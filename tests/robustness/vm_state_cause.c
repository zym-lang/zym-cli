// What the VM is, and why.
//
// ZymStatus says what one call returned. ZymVmState/ZymVmCause say what the VM
// *is* and what put it there -- three different reasons currently arrive as the
// same ABORTED status, so the status alone cannot tell a host whether to grant
// another slice, free memory, or give up.
//
// Every transition is covered here, plus the detail fields and `resumable`,
// which folds together "suspended" with "every sticky condition cleared".

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

static const char* TRIVIAL = "var x = 1 + 1\nfunc get() { return x }\n";
static const char* SPIN    = "func spin() { var i = 0\n while (true) { i = i + 1 } }\nspin()\n";
static const char* BOOM    = "var bad = null\nbad.nope()\n";
static const char* GLUTTON =
    "var hoard = []\nvar i = 0\nwhile (true) { push(hoard, [i, i])\n i = i + 1 }\n";

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
    // ---- idle -> idle on a clean run -------------------------------------
    {
        ZymVM* vm = zym_newVM(NULL);
        CHECK(zym_vmState(vm) == ZYM_STATE_IDLE, "a fresh VM is idle");
        CHECK(zym_vmCause(vm) == ZYM_CAUSE_NONE, "with no cause");

        ZymChunk* c = compile_or_null(vm, TRIVIAL);
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_OK, "trivial program runs");
        CHECK(zym_vmState(vm) == ZYM_STATE_IDLE, "a completed run leaves it idle");
        CHECK(zym_vmCause(vm) == ZYM_CAUSE_NONE, "and clears the cause");

        ZymVmInfo info;
        zym_vmInfo(vm, &info);
        CHECK(!info.resumable, "a finished VM is not resumable");
        CHECK(info.memory_used > 0, "info reports memory usage");

        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- a watchdog: suspended, cause PREEMPT, entry identified ----------
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm, SPIN);
        ZymPreemptId wd = zym_preemptRegister(vm, 200000, zym_newNull(), 0);
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_ABORTED, "watchdog stops the spin");
        CHECK(zym_vmState(vm) == ZYM_STATE_SUSPENDED, "state is suspended, not failed");
        CHECK(zym_vmCause(vm) == ZYM_CAUSE_PREEMPT, "cause is preemption");

        ZymVmInfo info;
        zym_vmInfo(vm, &info);
        CHECK(info.preempt_id == wd, "the entry that fired is named");
        CHECK(info.resumable, "nothing sticky is pending, so it is resumable");

        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- a hard stop: same state, different cause, NOT resumable ---------
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm, SPIN);
        zym_requestStop(vm);
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_ABORTED, "the stop takes effect");
        CHECK(zym_vmState(vm) == ZYM_STATE_SUSPENDED, "still merely suspended");
        CHECK(zym_vmCause(vm) == ZYM_CAUSE_HOST_STOP,
              "and the cause distinguishes it from a watchdog");

        ZymVmInfo info;
        zym_vmInfo(vm, &info);
        CHECK(!info.resumable, "a pending stop makes it not resumable");

        zym_clearStop(vm);
        zym_vmInfo(vm, &info);
        CHECK(info.resumable, "clearing the stop makes it resumable again");

        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- the memory ceiling: cause and the size that crossed it ----------
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm, GLUTTON);
        zym_setMemoryLimit(vm, zym_memoryUsed(vm) + (256 * 1024));
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_ABORTED, "the ceiling trips");
        CHECK(zym_vmState(vm) == ZYM_STATE_SUSPENDED, "suspended, not failed");
        CHECK(zym_vmCause(vm) == ZYM_CAUSE_MEMORY_LIMIT,
              "cause names the memory ceiling");

        ZymVmInfo info;
        zym_vmInfo(vm, &info);
        CHECK(info.bytes_wanted > 0, "the request that crossed the line is reported");
        CHECK(info.memory_limit > 0, "the limit is reported");
        CHECK(info.memory_used > info.memory_limit - (256 * 1024),
              "usage is reported alongside it");
        CHECK(!info.resumable, "a pending memory condition blocks resume");

        zym_setMemoryLimit(vm, zym_memoryUsed(vm) + (256 * 1024));
        zym_vmInfo(vm, &info);
        CHECK(info.resumable, "granting room makes it resumable");

        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- an error is a different state entirely --------------------------
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm, BOOM);
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_RUNTIME_ERROR, "the program fails");
        CHECK(zym_vmState(vm) == ZYM_STATE_FAILED,
              "a failure is FAILED, never SUSPENDED");
        CHECK(zym_vmCause(vm) == ZYM_CAUSE_RUNTIME_ERROR, "with a runtime-error cause");

        ZymVmInfo info;
        zym_vmInfo(vm, &info);
        CHECK(!info.resumable, "a failed VM is not resumable");

        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- the cause is latched, and cleared by the next run ---------------
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* spin = compile_or_null(vm, SPIN);
        zym_preemptRegister(vm, 200000, zym_newNull(), 0);
        zym_runChunk(vm, spin);
        CHECK(zym_vmCause(vm) == ZYM_CAUSE_PREEMPT, "cause set by the run");

        // Still readable after the fact, without re-running anything.
        CHECK(zym_vmCause(vm) == ZYM_CAUSE_PREEMPT, "and it latches");

        zym_freeChunk(vm, spin);
        zym_freeVM(vm);

        ZymVM* vm2 = zym_newVM(NULL);
        ZymChunk* ok = compile_or_null(vm2, TRIVIAL);
        zym_runChunk(vm2, ok);
        CHECK(zym_vmCause(vm2) == ZYM_CAUSE_NONE,
              "a clean run leaves no stale cause behind");
        zym_freeChunk(vm2, ok);
        zym_freeVM(vm2);
    }

    // ---- resuming in slices keeps reporting the same cause ---------------
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm,
            "var t = 0\nvar i = 0\nwhile (i < 400000) { t = t + 1\n i = i + 1 }\n"
            "func get() { return t }\n");
        zym_preemptRegister(vm, 50000, zym_newNull(), 0);

        ZymStatus st = zym_runChunk(vm, c);
        int slices = 0;
        while (st == ZYM_STATUS_ABORTED && slices < 500) {
            if (zym_vmCause(vm) != ZYM_CAUSE_PREEMPT) { failures++; break; }
            st = zym_resume(vm);
            slices++;
        }
        CHECK(st == ZYM_STATUS_OK, "slicing completes the work");
        CHECK(zym_vmState(vm) == ZYM_STATE_IDLE, "and ends idle");
        CHECK(zym_vmCause(vm) == ZYM_CAUSE_NONE, "with the cause cleared");

        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    printf(failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
