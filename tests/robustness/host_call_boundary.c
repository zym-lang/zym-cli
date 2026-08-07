// The host call boundary: what a native re-entering the VM may and may not do.
//
// A native that calls back into the VM (script -> C -> script) leaves its own
// C frame on the real stack underneath the script it started. Nothing in the
// VM can slice that frame: there is no protocol for unwinding a native and
// rebuilding it later. Natives are therefore ATOMIC with respect to anything
// that needs to save the stack for later.
//
// Two mechanisms need to save the stack, and they fail differently:
//
//   - CAPTURE has no correct later moment -- the capture point is now -- so a
//     continuation spanning a boundary is refused outright.
//   - SUSPENSION could in principle wait, but waiting for a boundary that may
//     never pop is how a watchdog gets defeated by the script it bounds. So a
//     suspension that would have to cross a boundary terminates instead: an
//     error already travels out through the status returns natives handle.
//
// What is explicitly still ALLOWED is preemption itself. A callback-bearing
// entry pushes a VM frame, runs, and the dispatch loop continues with the C
// frame untouched below it -- nothing crosses. That case is the host event
// pump, and breaking it would be a worse regression than the bugs above.
//
// Also covered: the frame window a re-entrant call reserves must include the
// caller's spill area, or the new frame lands inside live spilled locals.
//
// Every case is bounded, so a hang shows up as a runAll.sh timeout rather
// than an infinite test run.

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

static ZymChunk* compile_or_null(ZymVM* vm, const char* src) {
    ZymCompilerConfig cfg = { 1 };
    ZymChunk* chunk = zym_newChunk(vm);
    if (zym_compile(vm, src, chunk, NULL, "t.zym", cfg, NULL) != ZYM_STATUS_OK) {
        zym_freeChunk(vm, chunk);
        return NULL;
    }
    return chunk;
}

// The native under test: it re-enters the VM, the way a module-loader
// callback, a transaction wrapper, or a cross-VM trampoline does. It
// propagates a failed inner call, which is what a well-behaved native does --
// one that swallowed the status would hide every diagnostic below.
// What the native observed when its re-entrant call came back. The OUTER run
// status cannot tell the two apart: a propagating native turns a SUSPENDED
// inner call into an error just as the guard turns it into one. The
// difference is only visible here, before the native propagates.
static ZymStatus  inner_status;
static ZymVmState inner_state;
static ZymVmCause inner_cause;

static ZymValue reenter(ZymVM* vm, ZymValue ctx) {
    (void)ctx;
    inner_status = zym_call(vm, "inner", 0);
    inner_state  = zym_vmState(vm);
    inner_cause  = zym_vmCause(vm);
    if (inner_status != ZYM_STATUS_OK) return ZYM_ERROR;
    return zym_newNull();
}

// Requests the stop and THEN re-enters, so the stop is pending while script
// runs underneath the boundary. Requesting it before the run would trip at the
// first dispatch, before this native is ever reached.
static ZymValue stopThenReenter(ZymVM* vm, ZymValue ctx) {
    (void)ctx;
    zym_requestStop(vm);
    inner_status = zym_call(vm, "inner", 0);
    inner_state  = zym_vmState(vm);
    inner_cause  = zym_vmCause(vm);
    if (inner_status != ZYM_STATUS_OK) return ZYM_ERROR;
    return zym_newNull();
}

static int ticks = 0;
static ZymValue tick(ZymVM* vm, ZymValue ctx) {
    (void)vm; (void)ctx;
    ticks++;
    return zym_newNull();
}

int main(void) {
    // ---- capture across a boundary is refused ---------------------------
    {
        ZymVM* vm = zym_newVM(NULL);
        zym_defineNative(vm, "reenter()", (void*)reenter);
        ZymChunk* c = compile_or_null(vm,
            "var TAG = Cont.newPrompt()\n"
            "func inner() { return Cont.shift(TAG, func(k) { return 0 }) }\n"
            "func go() { return Cont.withPrompt(TAG, func() { reenter()\n return 1 }) }\n"
            "go()\n");
        CHECK(c != NULL, "the capture-across-a-native program compiles");
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_RUNTIME_ERROR,
              "capturing a continuation across a native's C frame is refused");
        CHECK(zym_vmState(vm) == ZYM_STATE_FAILED,
              "and the refusal is a failure, not a suspension");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- the same shape with no native still captures --------------------
    // The guard must key on a live C frame, not on "a continuation was used".
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm,
            "var TAG = Cont.newPrompt()\n"
            "func inner() { return Cont.shift(TAG, func(k) { return 0 }) }\n"
            "func go() { return Cont.withPrompt(TAG, func() { inner()\n return 1 }) }\n"
            "go()\n");
        CHECK(c != NULL, "the control program compiles");
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_OK,
              "an ordinary capture with no C frame in the way still works");
        zym_freeVM(vm);
    }

    // ---- a watchdog under a boundary terminates rather than suspending ---
    // Suspending here would leave the VM claiming resumable while a resume
    // returns through a native whose C frame has unwound.
    {
        ZymVM* vm = zym_newVM(NULL);
        zym_defineNative(vm, "reenter()", (void*)reenter);
        ZymChunk* c = compile_or_null(vm,
            "func inner() { var i = 0\n"
            " while (i < 100000000) { i = i + 1 }\n"
            " return i }\n"
            "reenter()\n");
        CHECK(c != NULL, "the watchdog-under-a-native program compiles");
        inner_status = ZYM_STATUS_OK;
        zym_preemptRegister(vm, 200000, zym_newNull(), 0);
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_RUNTIME_ERROR,
              "a watchdog that would have to cross a native terminates");
        // The discriminating assertions: without the guard the inner call
        // comes back SUSPENDED/PREEMPT and the native is handed a VM that
        // claims to be resumable through a frame it cannot rebuild.
        CHECK(inner_status == ZYM_STATUS_RUNTIME_ERROR,
              "the native's re-entrant call reports an error, not a suspension");
        CHECK(inner_state == ZYM_STATE_FAILED,
              "the VM the native is handed back is FAILED, not SUSPENDED");
        CHECK(inner_cause != ZYM_CAUSE_PREEMPT,
              "and does not report a resumable preempt cause");
        CHECK(zym_vmState(vm) == ZYM_STATE_FAILED,
              "leaving the VM FAILED, never SUSPENDED");

        ZymVmInfo info;
        zym_vmInfo(vm, &info);
        CHECK(!info.resumable,
              "and never reporting resumable for a stack it cannot rebuild");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- the same watchdog with no native suspends as before -------------
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm,
            "var i = 0\nwhile (i < 100000000) { i = i + 1 }\n");
        CHECK(c != NULL, "the plain-watchdog program compiles");
        zym_preemptRegister(vm, 200000, zym_newNull(), 0);
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_SUSPENDED,
              "a watchdog with no boundary in the way still suspends");
        CHECK(zym_vmState(vm) == ZYM_STATE_SUSPENDED, "with a suspended state");
        CHECK(zym_vmCause(vm) == ZYM_CAUSE_PREEMPT, "and a preempt cause");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- a host stop under a boundary also terminates --------------------
    {
        ZymVM* vm = zym_newVM(NULL);
        zym_defineNative(vm, "stopThenReenter()", (void*)stopThenReenter);
        ZymChunk* c = compile_or_null(vm,
            "func inner() { var i = 0\n"
            " while (i < 100000000) { i = i + 1 }\n"
            " return i }\n"
            "stopThenReenter()\n");
        CHECK(c != NULL, "the stop-under-a-native program compiles");
        inner_status = ZYM_STATUS_OK;
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_RUNTIME_ERROR,
              "a host stop that would have to cross a native terminates too");
        CHECK(inner_status == ZYM_STATUS_RUNTIME_ERROR,
              "the stop reaches the native as an error, not a suspension");
        CHECK(inner_state == ZYM_STATE_FAILED,
              "and not as a parked VM the native might try to resume");
        CHECK(zym_vmState(vm) == ZYM_STATE_FAILED,
              "and the stop leaves it FAILED rather than parked");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- preemption UNDER a boundary still runs in place -----------------
    // The case the guard must not catch: a callback-bearing entry pushes a VM
    // frame and the loop continues. Nothing crosses the C frame, so this is
    // the host event pump and it has to keep working.
    {
        ticks = 0;
        ZymVM* vm = zym_newVM(NULL);
        zym_defineNative(vm, "reenter()", (void*)reenter);
        zym_defineNative(vm, "onTick()", (void*)tick);
        ZymChunk* c = compile_or_null(vm,
            "func handler() { onTick() }\n"
            "func inner() { var i = 0\n"
            " while (i < 3000000) { i = i + 1 }\n"
            " return i }\n"
            "Preempt.every(200000, handler)\n"
            "reenter()\n");
        CHECK(c != NULL, "the event-pump program compiles");
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_OK,
              "a callback-bearing entry under a native runs to completion");
        CHECK(ticks > 1,
              "and its callback fired repeatedly in place, without crossing");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- a re-entrant call reserves the caller's spill area --------------
    // zym_callClosurev sized its frame as max_regs alone while every other
    // frame-push site used max_regs + spill_count, so a re-entrant call could
    // land inside the caller's spilled locals. The symptom is a live local
    // changing value across the native call, so that is what is asserted.
    {
        ZymVM* vm = zym_newVM(NULL);
        zym_defineNative(vm, "reenter()", (void*)reenter);
        ZymChunk* c = compile_or_null(vm,
            "func inner() { var a = 1\n var b = 2\n return a + b }\n"
            "func outer() {\n"
            "  var keep = 1234\n"
            "  var also = 5678\n"
            "  reenter()\n"
            "  return keep + also\n"
            "}\n");
        CHECK(c != NULL, "the spill program compiles");
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_OK, "it defines cleanly");
        CHECK(zym_call(vm, "outer", 0) == ZYM_STATUS_OK,
              "a native re-entering mid-function returns cleanly");
        CHECK(zym_asNumber(zym_getCallResult(vm)) == 6912.0,
              "and the caller's locals survive the re-entrant call");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    printf(failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
