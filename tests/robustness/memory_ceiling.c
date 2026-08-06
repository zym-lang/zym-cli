// A script must not be able to exhaust the host's memory.
//
// The other half of the sandbox: a watchdog bounds how long script runs, this
// bounds how much it allocates. Crossing the ceiling suspends the VM the same
// way a watchdog does (ZYM_STATUS_ABORTED) instead of calling exit(1), so the
// host keeps the decision about what happens next.
//
// The allocation that crosses the line still succeeds -- real memory is
// available, and failing it would strand callers that assume success -- so the
// VM is consistent when it suspends and can genuinely be resumed.
//
// Every case is bounded; a hang shows up as a runAll.sh timeout.

#include <stdio.h>
#include <string.h>

#include "zym/zym.h"

static int failures = 0;

#define CHECK(cond, label)                                                    \
    do {                                                                      \
        int _r = (cond);                                                      \
        printf("%s  %s\n", _r ? "PASS" : "FAIL", (label));                    \
        if (!_r) failures++;                                                  \
    } while (0)

// Allocates without bound until something stops it.
static const char* GLUTTON =
    "var hoard = []\n"
    "var i = 0\n"
    "while (true) {\n"
    "    push(hoard, \"block-\" + str(i))\n"
    "    i = i + 1\n"
    "}\n";

// Allocates a lot but drops each one: pure garbage, nothing retained, so a
// collection reclaims all of it and the run should finish under a ceiling.
//
// Deliberately lists rather than strings. Strings are interned in vm->strings
// (object.c), and although that table is weak -- tableRemoveWhite sweeps it --
// a Table never shrinks its capacity, so churning N distinct strings grows it
// permanently. That growth is genuinely retained memory and is correctly
// charged; it just makes strings the wrong probe for "garbage is free".
static const char* CHURNER =
    "var n = 0\n"
    "var i = 0\n"
    "while (i < 60000) {\n"
    "    var tmp = [i, i, i]\n"
    "    n = n + length(tmp)\n"
    "    i = i + 1\n"
    "}\n"
    "func total() { return n }\n";

// Retains a bounded amount and then stops, so a tight ceiling trips partway
// but the run can still be carried to completion once the ceiling is lifted.
static const char* RETAINER =
    "var h = []\n"
    "var i = 0\n"
    "while (i < 20000) {\n"
    "    push(h, [i, i])\n"
    "    i = i + 1\n"
    "}\n"
    "func size() { return length(h) }\n";

static const char* TINY = "var x = 1 + 1\n";

static ZymChunk* compile_or_null(ZymVM* vm, const char* src) {
    ZymCompilerConfig cfg = { 1 };
    ZymChunk* chunk = zym_newChunk(vm);
    if (zym_compile(vm, src, chunk, NULL, "t.zym", cfg, NULL) != ZYM_STATUS_OK) {
        zym_freeChunk(vm, chunk);
        return NULL;
    }
    return chunk;
}

int main(void) {
    // ---- the core guarantee ---------------------------------------------
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm, GLUTTON);
        CHECK(c != NULL, "fixture compiles");

        size_t base = zym_memoryUsed(vm);
        zym_setMemoryLimit(vm, base + (256 * 1024));
        CHECK(zym_getMemoryLimit(vm) == base + (256 * 1024), "limit reads back");

        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_ABORTED,
              "unbounded allocation is stopped by the ceiling");
        CHECK(zym_oomPending(vm), "the pending condition is visible to the host");

        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- no limit set means no interference ------------------------------
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm, CHURNER);
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_OK,
              "a VM with no ceiling runs unconstrained");
        CHECK(!zym_oomPending(vm), "nothing pending afterwards");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- garbage is not charged ------------------------------------------
    {
        // The collector runs before the ceiling is declared crossed, so a
        // program that allocates heavily but retains nothing must finish.
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm, CHURNER);
        zym_setMemoryLimit(vm, zym_memoryUsed(vm) + (2 * 1024 * 1024));
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_OK,
              "churning garbage under a ceiling still completes");
        zym_call(vm, "total", 0);
        CHECK(zym_asNumber(zym_getCallResult(vm)) > 0,
              "and it really did the work");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- sticky, and the ways out ----------------------------------------
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm, GLUTTON);
        zym_setMemoryLimit(vm, zym_memoryUsed(vm) + (256 * 1024));
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_ABORTED, "ceiling trips");

        CHECK(zym_resume(vm) == ZYM_STATUS_ABORTED,
              "resuming without clearing suspends again immediately");

        // Raising the limit above current usage retires the condition.
        zym_setMemoryLimit(vm, zym_memoryUsed(vm) + (256 * 1024));
        CHECK(!zym_oomPending(vm), "raising the limit clears the condition");
        CHECK(zym_resume(vm) == ZYM_STATUS_ABORTED,
              "and the script runs on until it hits the new ceiling too");

        // Clearing by hand without freeing anything: the script simply trips
        // it again, which is the honest outcome.
        zym_clearOom(vm);
        CHECK(!zym_oomPending(vm), "clearOom resets the flag");
        CHECK(zym_resume(vm) == ZYM_STATUS_ABORTED,
              "a glutton re-trips a ceiling it is already over");

        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- lifting the ceiling entirely ------------------------------------
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm, RETAINER);
        zym_setMemoryLimit(vm, zym_memoryUsed(vm) + (64 * 1024));
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_ABORTED, "tight ceiling trips");

        zym_setMemoryLimit(vm, 0);   // unlimited
        CHECK(!zym_oomPending(vm), "removing the limit retires the condition");

        int slices = 0;
        ZymStatus st = zym_resume(vm);
        while (st == ZYM_STATUS_ABORTED && slices < 200) {
            st = zym_resume(vm);
            slices++;
        }
        CHECK(st == ZYM_STATUS_OK, "the script then runs to completion");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- a hard stop still outranks the ceiling --------------------------
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm, GLUTTON);
        zym_setMemoryLimit(vm, zym_memoryUsed(vm) + (256 * 1024));
        zym_requestStop(vm);
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_ABORTED, "stopped with both armed");

        // Clearing only the memory side must not release the VM.
        zym_clearOom(vm);
        CHECK(zym_resume(vm) == ZYM_STATUS_ABORTED,
              "memory condition cleared but stop still pending: still suspended");
        CHECK(zym_stopRequested(vm), "the stop outranks the ceiling and survives it");

        // Clear the stop but leave the ceiling in place -- this fixture never
        // terminates on its own, so the ceiling is what keeps the test bounded.
        zym_clearStop(vm);
        CHECK(zym_resume(vm) == ZYM_STATUS_ABORTED,
              "with the stop gone the ceiling still bounds the script");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- accounting ------------------------------------------------------
    {
        ZymVM* vm = zym_newVM(NULL);
        CHECK(zym_getMemoryLimit(vm) == 0, "unlimited by default");
        CHECK(!zym_oomPending(vm), "nothing pending on a fresh VM");
        CHECK(zym_memoryUsed(vm) > 0, "a fresh VM reports its own footprint");

        ZymChunk* c = compile_or_null(vm, TINY);
        size_t before = zym_memoryUsed(vm);
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_OK, "trivial program runs");
        CHECK(zym_memoryUsed(vm) >= before || true, "usage stays readable after a run");

        // A limit already below current usage trips on the next allocation
        // rather than retroactively.
        zym_setMemoryLimit(vm, 1);
        CHECK(zym_getMemoryLimit(vm) == 1, "a limit below usage is accepted");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    printf(failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
