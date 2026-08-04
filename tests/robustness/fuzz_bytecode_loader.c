// Mutation fuzzer for the bytecode loader.
//
// Contract under test: zym_deserializeChunk must ALWAYS either accept its
// input or return an error. It must never crash, never write through a
// pointer derived from file data, never recurse without bound, and never
// let a count field drive an allocation the allocator will die on —
// reallocate() treats allocation failure as fatal and calls exit(1), so an
// unchecked count terminates the host process instead of rejecting a file.
//
// Every stage below is deterministic (fixed-seed xorshift), so a failure
// reproduces exactly. A crash shows up as a non-zero exit status from the
// signal, which runAll.sh reports as a failure.
//
// SCOPE: this fuzzer stops at the loader. It does not execute the mutants
// it accepts. A chunk can be structurally valid — every count, length and
// pool index in range — while its instruction stream is garbage; executing
// that still crashes, because opcodes, register indices, constant indices
// and jump targets are never validated. That needs a bytecode verifier.
// See README.md. When one exists, run survivors and the gap closes.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifndef _WIN32
    #include <fcntl.h>
    #include <unistd.h>
#endif

#include "zym/zym.h"

// The loader reports why it rejected each input on stderr, which is what
// you want when a real .zbc fails to load and pure noise when you are
// rejecting thousands of mutants. Mute it around the fuzz loops only.
static int stderr_saved = -1;

static void quiet_begin(void) {
#ifndef _WIN32
    fflush(stderr);
    stderr_saved = dup(fileno(stderr));
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull != -1) {
        dup2(devnull, fileno(stderr));
        close(devnull);
    }
#endif
}

static void quiet_end(void) {
#ifndef _WIN32
    fflush(stderr);
    if (stderr_saved != -1) {
        dup2(stderr_saved, fileno(stderr));
        close(stderr_saved);
        stderr_saved = -1;
    }
#endif
}

// Exercises every serialized constant kind: enum schema + enum value,
// struct schema, nested functions with upvalues, strings, numbers.
static const char* SRC =
    "enum Grade { A, B, C }\n"
    "struct Rec { id; name }\n"
    "func helper(n){ return n + 1 }\n"
    "func outer(){ func inner(x){ return x * 2 }\n"
    "              return inner(helper(1)) }\n"
    "func main(argv){ var r = Rec { .id = 1, .name = \"n\" }\n"
    "                 var g = Grade.B\n"
    "                 return outer() + r.id }\n"
    "var v = main([])\n";

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static uint32_t next_rand(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 32);
}

// Load `buf` into a throwaway VM. Returns 1 if the loader accepted it.
// Reaching the return at all is the property being tested.
static int try_load(const char* buf, size_t len) {
    ZymVM* vm = zym_newVM(NULL);
    ZymChunk* chunk = zym_newChunk(vm);
    int accepted = (zym_deserializeChunk(vm, chunk, buf, len) == ZYM_STATUS_OK);
    zym_freeChunk(vm, chunk);
    zym_freeVM(vm);
    return accepted;
}

int main(int argc, char** argv) {
    long iterations = (argc > 1) ? strtol(argv[1], NULL, 10) : 4000;
    if (iterations < 1) iterations = 4000;

    ZymCompilerConfig cfg = { 1 };
    ZymVM* vm = zym_newVM(NULL);
    ZymChunk* chunk = zym_newChunk(vm);
    if (zym_compile(vm, SRC, chunk, NULL, "fuzz.zym", cfg, NULL) != ZYM_STATUS_OK) {
        printf("FAIL  could not compile fixture\n");
        return 1;
    }
    char* base = NULL;
    size_t size = 0;
    if (zym_serializeChunk(vm, cfg, chunk, &base, &size) != ZYM_STATUS_OK) {
        printf("FAIL  could not serialize fixture\n");
        return 1;
    }
    zym_freeChunk(vm, chunk);
    zym_freeVM(vm);

    if (!try_load(base, size)) {
        printf("FAIL  baseline bytecode does not load\n");
        free(base);
        return 1;
    }
    printf("      baseline: %zu bytes, loads cleanly\n", size);
    fflush(stdout);
    quiet_begin();

    char* mutant = (char*)malloc(size);
    if (!mutant) { printf("FAIL  out of memory\n"); free(base); return 1; }
    long accepted, rejected;

    // 1. Random byte mutations.
    accepted = rejected = 0;
    for (long i = 0; i < iterations; i++) {
        memcpy(mutant, base, size);
        int edits = 1 + (int)(next_rand() % 3);
        for (int e = 0; e < edits; e++) {
            mutant[next_rand() % size] = (char)(next_rand() & 0xFF);
        }
        try_load(mutant, size) ? accepted++ : rejected++;
    }
    printf("PASS  random byte mutations      (%ld accepted, %ld rejected, 0 crashes)\n",
           accepted, rejected);
    fflush(stdout);

    // 2. Hostile 32-bit values at every aligned offset. This is the stage
    //    that targets count and index fields directly — the ones that size
    //    allocations or index the string pool.
    static const int32_t hostile[] = {
        -1, INT32_MAX, INT32_MIN, 0x7FFFFFF0, -2, 65536, 0x00FFFFFF
    };
    accepted = rejected = 0;
    for (size_t off = 0; off + sizeof(int32_t) <= size; off++) {
        for (size_t h = 0; h < sizeof(hostile) / sizeof(hostile[0]); h++) {
            memcpy(mutant, base, size);
            memcpy(mutant + off, &hostile[h], sizeof(int32_t));
            try_load(mutant, size) ? accepted++ : rejected++;
        }
    }
    printf("PASS  hostile i32 at every offset (%ld accepted, %ld rejected, 0 crashes)\n",
           accepted, rejected);
    fflush(stdout);

    // 3. Every truncation length. A short file must never be read past its
    //    end, whatever its header claims.
    accepted = rejected = 0;
    for (size_t len = 0; len < size; len++) {
        try_load(base, len) ? accepted++ : rejected++;
    }
    printf("PASS  truncations                 (%ld accepted, %ld rejected, 0 crashes)\n",
           accepted, rejected);
    fflush(stdout);

    quiet_end();
    free(mutant);
    free(base);

    printf("      NOTE: loader safety only. Executing structurally-valid but\n"
           "      semantically-corrupt bytecode still crashes; that needs a\n"
           "      bytecode verifier (see README.md).\n");
    printf("ALL PASS\n");
    return 0;
}
