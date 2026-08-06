// The allocator genuinely running out must not kill the host.
//
// The memory ceiling handles the case where memory IS available and the host
// simply declined to hand more over: the allocation succeeds and the VM
// suspends, resumable. This file covers the other case, where the allocator
// itself fails and a collection cannot free enough.
//
// That one cannot be made resumable. reallocate must return usable memory to
// callers that assume success, so the only honest alternative to killing the
// process is to leave the operation entirely: the VM unwinds to the nearest
// API boundary and reports ZYM_STATUS_RUNTIME_ERROR with cause
// ZYM_CAUSE_OUT_OF_MEMORY, in state FAILED rather than SUSPENDED. The host can
// then free the VM, which is the only sound thing to do with it.
//
// Driven through a test allocator that fails after a set number of successful
// allocations, so the failure lands at an arbitrary interior point rather than
// somewhere convenient.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zym/zym.h"

static int failures = 0;

#define CHECK(cond, label)                                                    \
    do {                                                                      \
        int _r = (cond);                                                      \
        printf("%s  %s\n", _r ? "PASS" : "FAIL", (label));                    \
        if (!_r) failures++;                                                  \
    } while (0)

// ---- a allocator that stops giving after N successes ----------------------
typedef struct {
    long budget;      // successful allocations remaining; <0 means unlimited
    long served;
} Starver;

static void* st_alloc(void* ctx, size_t size) {
    Starver* s = (Starver*)ctx;
    if (s->budget == 0) return NULL;
    if (s->budget > 0) s->budget--;
    s->served++;
    return malloc(size);
}
static void* st_calloc(void* ctx, size_t count, size_t size) {
    Starver* s = (Starver*)ctx;
    if (s->budget == 0) return NULL;
    if (s->budget > 0) s->budget--;
    s->served++;
    return calloc(count, size);
}
static void* st_realloc(void* ctx, void* ptr, size_t old_size, size_t new_size) {
    (void)old_size;
    Starver* s = (Starver*)ctx;
    if (new_size == 0) { free(ptr); return NULL; }
    if (s->budget == 0) return NULL;
    if (s->budget > 0) s->budget--;
    s->served++;
    return realloc(ptr, new_size);
}
static void st_free(void* ctx, void* ptr, size_t size) {
    (void)ctx; (void)size;
    free(ptr);
}

static ZymAllocator make_allocator(Starver* s) {
    ZymAllocator a;
    a.alloc   = st_alloc;
    a.calloc  = st_calloc;
    a.realloc = st_realloc;
    a.free    = st_free;
    a.ctx     = s;
    return a;
}

// Allocates steadily, so the starving allocator trips somewhere inside it.
static const char* HUNGRY =
    "var keep = []\n"
    "var i = 0\n"
    "while (i < 200000) {\n"
    "    push(keep, [i, i, i])\n"
    "    i = i + 1\n"
    "}\n";

static const char* TRIVIAL = "var x = 1 + 1\nfunc get() { return x }\n";

int main(void) {
    // ---- baseline: unlimited budget behaves normally ---------------------
    {
        Starver s = { -1, 0 };
        ZymAllocator a = make_allocator(&s);
        ZymVM* vm = zym_newVM(&a);
        CHECK(vm != NULL, "a VM builds on the test allocator");

        ZymCompilerConfig cfg = { 1 };
        ZymChunk* c = zym_newChunk(vm);
        CHECK(zym_compile(vm, TRIVIAL, c, NULL, "t.zym", cfg, NULL) == ZYM_STATUS_OK,
              "and compiles normally");
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_OK, "and runs normally");
        CHECK(zym_vmCause(vm) == ZYM_CAUSE_NONE, "with no cause");

        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- the allocator gives out mid-run ---------------------------------
    {
        Starver s = { -1, 0 };
        ZymAllocator a = make_allocator(&s);
        ZymVM* vm = zym_newVM(&a);

        ZymCompilerConfig cfg = { 1 };
        ZymChunk* c = zym_newChunk(vm);
        CHECK(zym_compile(vm, HUNGRY, c, NULL, "t.zym", cfg, NULL) == ZYM_STATUS_OK,
              "hungry fixture compiles while memory is available");

        // Starve it from here: everything up to now succeeded, so the failure
        // lands inside the run rather than during setup.
        s.budget = 200;

        ZymStatus st = zym_runChunk(vm, c);

        CHECK(st == ZYM_STATUS_RUNTIME_ERROR,
              "a genuine allocation failure returns instead of killing the process");
        CHECK(zym_vmCause(vm) == ZYM_CAUSE_OUT_OF_MEMORY,
              "and names out-of-memory as the cause");
        CHECK(zym_vmState(vm) == ZYM_STATE_FAILED,
              "state is FAILED, not SUSPENDED: the frames are not continuable");

        ZymVmInfo info;
        zym_vmInfo(vm, &info);
        CHECK(!info.resumable, "and it does not claim to be resumable");

        // Give it room again purely so teardown can do its work.
        s.budget = -1;
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
        CHECK(1, "the VM tears down afterwards without crashing");
    }

    // ---- failing at a different point ------------------------------------
    {
        // A tighter budget trips earlier, in a different allocation site.
        Starver s = { -1, 0 };
        ZymAllocator a = make_allocator(&s);
        ZymVM* vm = zym_newVM(&a);

        ZymCompilerConfig cfg = { 1 };
        ZymChunk* c = zym_newChunk(vm);
        zym_compile(vm, HUNGRY, c, NULL, "t.zym", cfg, NULL);

        s.budget = 20;
        ZymStatus st = zym_runChunk(vm, c);
        CHECK(st == ZYM_STATUS_RUNTIME_ERROR, "an earlier failure also returns");
        CHECK(zym_vmCause(vm) == ZYM_CAUSE_OUT_OF_MEMORY, "with the same cause");

        s.budget = -1;
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- the ceiling and a real failure are different things -------------
    {
        // A ceiling breach is SUSPENDED and resumable; a real failure is
        // FAILED and is not. A host that conflates them would try to resume a
        // VM whose frames are mid-operation.
        Starver s = { -1, 0 };
        ZymAllocator a = make_allocator(&s);
        ZymVM* vm = zym_newVM(&a);

        ZymCompilerConfig cfg = { 1 };
        ZymChunk* c = zym_newChunk(vm);
        zym_compile(vm, HUNGRY, c, NULL, "t.zym", cfg, NULL);

        zym_setMemoryLimit(vm, zym_memoryUsed(vm) + (128 * 1024));
        ZymStatus st = zym_runChunk(vm, c);
        CHECK(st == ZYM_STATUS_SUSPENDED, "the ceiling suspends rather than fails");
        CHECK(zym_vmCause(vm) == ZYM_CAUSE_MEMORY_LIMIT, "with MEMORY_LIMIT");
        CHECK(zym_vmState(vm) == ZYM_STATE_SUSPENDED, "in SUSPENDED state");

        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- failure during COMPILE, which has no instruction boundary -------
    {
        // A compile allocates heavily and cannot suspend partway, so it needs
        // its own recovery boundary. Without one this was a process kill.
        Starver s = { -1, 0 };
        ZymAllocator a = make_allocator(&s);
        ZymVM* vm = zym_newVM(&a);

        ZymCompilerConfig cfg = { 1 };
        ZymChunk* c = zym_newChunk(vm);

        s.budget = 40;   // trips inside the compiler
        ZymStatus st = zym_compile(vm, HUNGRY, c, NULL, "t.zym", cfg, NULL);
        CHECK(st == ZYM_STATUS_COMPILE_ERROR,
              "an allocation failure during compile reports a compile error");
        CHECK(zym_vmCause(vm) == ZYM_CAUSE_OUT_OF_MEMORY,
              "with out-of-memory as the cause");

        s.budget = -1;
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
        CHECK(1, "and teardown is clean");
    }

    // ---- the ceiling bounds a compile too --------------------------------
    {
        // The ceiling normally suspends at an instruction boundary, which a
        // compile never reaches. It reuses the frontend's cancellation poll
        // instead, so a runaway compile is bounded rather than unbounded.
        Starver s = { -1, 0 };
        ZymAllocator a = make_allocator(&s);
        ZymVM* vm = zym_newVM(&a);

        ZymCompilerConfig cfg = { 1 };
        ZymChunk* c = zym_newChunk(vm);

        // A source large enough that compiling it is itself the allocation.
        // HUNGRY allocates at RUN time; this one allocates at COMPILE time,
        // which is the case with no instruction boundary to stop at.
        size_t n = 40000;
        char* big = (char*)malloc(n * 24 + 1);
        CHECK(big != NULL, "big source allocates");
        size_t off = 0;
        for (size_t i = 0; i < n; i++) {
            off += (size_t)sprintf(big + off, "var v%zu = %zu\n", i, i);
        }

        zym_setMemoryLimit(vm, zym_memoryUsed(vm) + (64 * 1024));
        ZymStatus st = zym_compile(vm, big, c, NULL, "t.zym", cfg, NULL);
        CHECK(st == ZYM_STATUS_COMPILE_ERROR,
              "a compile that crosses the memory ceiling stops instead of running away");
        free(big);

        zym_setMemoryLimit(vm, 0);
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    printf(failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
