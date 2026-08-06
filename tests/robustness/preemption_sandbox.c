// Preemption and the host stop guarantee.
//
// Contract under test: a host can ALWAYS stop a running script, and script
// code cannot escape or suppress that. This is the resource half of the
// sandbox; capability gating is the other half and is not covered here.
//
// The properties that matter:
//   - a watchdog stops an infinite loop
//   - `Preempt.shield(...)` inside the script does NOT suppress a
//     non-maskable host watchdog (a shield masks only the script's own
//     maskable entries)
//   - an abort SUSPENDS rather than unwinds, so the host may resume
//   - whatever caused the stop stays in force until the host clears it
//   - clearing everything lets the script run free
//   - misuse (resuming a VM that already finished) reports an error
//     instead of crashing
//
// Every case is bounded, so a hang shows up as a runAll.sh timeout rather
// than an infinite test run.

#include <stdio.h>
#include <string.h>

#include "zym/zym.h"

static int failures = 0;

// Evaluate the condition exactly once: several of these have side effects.
#define CHECK(cond, label)                                                    \
    do {                                                                      \
        int _r = (cond);                                                      \
        printf("%s  %s\n", _r ? "PASS" : "FAIL", (label));                    \
        if (!_r) failures++;                                                  \
    } while (0)

// A loop long enough that a small watchdog slice always interrupts it, but
// finite so a successful resume can actually complete.
static const char* WORK =
    "var total = 0\n"
    "for (var i = 0; i < 200000; i = i + 1) { total = total + 1 }\n"
    "func get() { return total }\n";

static const char* SPIN_FOREVER =
    "func spin() { var i = 0\n while (true) { i = i + 1 }\n }\n"
    "spin()\n";

// A runaway loop that wraps itself in a shield, i.e. a script actively
// trying to make itself unstoppable.
static const char* SPIN_SHIELDED =
    "func spin() { var i = 0\n while (true) { i = i + 1 }\n }\n"
    "Preempt.shield(spin)\n";

// Script-owned entries. These exercise the callback path rather than the
// abort path: an entry with a callback runs script and then has to be put back
// into a sane state, which is where both of the bugs below lived.
static const char* REARM_SRC =
    "var fires = 0\n"
    "var id = Preempt.every(10000, func() { fires = fires + 1 })\n"
    "var i = 0\n"
    "while (i < 500000) { i = i + 1 }\n"
    "Preempt.cancel(id)\n"
    "func get() { return fires }\n";

static const char* ONESHOT_SRC =
    "var fires = 0\n"
    "var id = Preempt.once(10000, func() { fires = fires + 1 })\n"
    "var i = 0\n"
    "while (i < 500000) { i = i + 1 }\n"
    "func get() { return fires }\n"
    "func rem() { return Preempt.remaining(id) }\n";

static const char* SHIELD_RELEASE_SRC =
    "var fires = 0\n"
    "var id = Preempt.every(10000, func() { fires = fires + 1 })\n"
    "func quiet() { var j = 0\n"
    "               while (j < 50000) { j = j + 1 }\n"
    "               return 0 }\n"
    "Preempt.shield(quiet)\n"
    "var mark = fires\n"
    "var k = 0\n"
    "while (k < 300000) { k = k + 1 }\n"
    "Preempt.cancel(id)\n"
    "func afterShield() { return fires - mark }\n";

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
    // ---- the core guarantee -------------------------------------------
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm, SPIN_FOREVER);
        CHECK(c != NULL, "fixture compiles");
        ZymPreemptId wd = zym_preemptRegister(vm, 200000, zym_newNull(), 0);
        CHECK(wd != 0, "watchdog registers");
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_ABORTED,
              "infinite loop is stopped by the watchdog");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm, SPIN_SHIELDED);
        zym_preemptRegister(vm, 200000, zym_newNull(), 0);
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_ABORTED,
              "a script shield cannot suppress a non-maskable watchdog");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm, WORK);
        zym_preemptRegister(vm, 100000000, zym_newNull(), 0);
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_OK,
              "a generous watchdog leaves normal programs alone");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm, WORK);
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_OK,
              "a VM with no entries at all runs unconstrained");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- explicit stop -------------------------------------------------
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm, WORK);
        zym_requestStop(vm);
        CHECK(zym_stopRequested(vm), "stopRequested reflects a pending stop");
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_ABORTED,
              "a pending stop aborts the run");
        CHECK(zym_resume(vm) == ZYM_STATUS_ABORTED,
              "the stop is sticky: resuming without clearing re-aborts");
        zym_clearStop(vm);
        CHECK(!zym_stopRequested(vm), "clearStop resets the flag");
        CHECK(zym_resume(vm) == ZYM_STATUS_OK,
              "clearStop then resume runs to completion");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- resume: an abort suspends, it does not unwind -----------------
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm, WORK);
        ZymPreemptId wd = zym_preemptRegister(vm, 50000, zym_newNull(), 0);
        ZymStatus st = zym_runChunk(vm, c);
        CHECK(st == ZYM_STATUS_ABORTED, "watchdog interrupts partway through");

        // A rearming watchdog grants one fresh slice per resume, so the work
        // completes in slices. Bounded so a regression cannot spin forever.
        int slices = 0;
        while (st == ZYM_STATUS_ABORTED && slices < 500) {
            st = zym_resume(vm);
            slices++;
        }
        CHECK(st == ZYM_STATUS_OK, "repeated resume completes the work");
        zym_preemptUnregister(vm, wd);
        zym_call(vm, "get", 0);
        CHECK(zym_asNumber(zym_getCallResult(vm)) == 200000,
              "the work actually finished, not just returned OK");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm, WORK);
        zym_preemptRegister(vm, 50000, zym_newNull(), ZYM_PREEMPT_ONESHOT);
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_ABORTED, "oneshot watchdog fires");
        CHECK(zym_resume(vm) == ZYM_STATUS_OK,
              "a oneshot retires after firing, so one resume finishes");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- letting the script run free -----------------------------------
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm, WORK);
        ZymPreemptId a = zym_preemptRegister(vm, 50000, zym_newNull(), 0);
        ZymPreemptId b = zym_preemptRegister(vm, 90000, zym_newNull(), 0);
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_ABORTED,
              "the nearest of several watchdogs fires first");
        zym_preemptUnregister(vm, a);
        zym_preemptUnregister(vm, b);
        CHECK(zym_resume(vm) == ZYM_STATUS_OK,
              "clearing every watchdog lets the script run free in one go");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }
    {
        // Both mechanisms armed: clearing only one must not be enough.
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm, WORK);
        ZymPreemptId wd = zym_preemptRegister(vm, 50000, zym_newNull(), 0);
        zym_requestStop(vm);
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_ABORTED, "stopped with both armed");
        zym_preemptUnregister(vm, wd);
        CHECK(zym_resume(vm) == ZYM_STATUS_ABORTED,
              "watchdog gone but stop still pending: still aborts");
        zym_clearStop(vm);
        CHECK(zym_resume(vm) == ZYM_STATUS_OK, "clearing both lets it run free");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- table bookkeeping ---------------------------------------------
    {
        ZymVM* vm = zym_newVM(NULL);
        int cap = zym_preemptCapacity();
        CHECK(cap >= 2, "capacity is reported");
        ZymPreemptId ids[64];
        int n = 0;
        for (int i = 0; i < cap && n < (int)(sizeof(ids)/sizeof(ids[0])); i++) {
            ZymPreemptId id = zym_preemptRegister(vm, 1000, zym_newNull(), 0);
            if (id) ids[n++] = id;
        }
        CHECK(n == cap, "the table fills to exactly its capacity");
        CHECK(zym_preemptRegister(vm, 1000, zym_newNull(), 0) == 0,
              "registration fails cleanly when full");
        CHECK(zym_preemptUnregister(vm, ids[0]), "unregister frees a slot");
        CHECK(zym_preemptRegister(vm, 1000, zym_newNull(), 0) != 0,
              "the freed slot is reusable");
        zym_freeVM(vm);
    }

    // ---- misuse must not crash ------------------------------------------
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm, "var x = 1 + 1\n");
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_OK, "trivial program runs");
        CHECK(zym_resume(vm) == ZYM_STATUS_RUNTIME_ERROR,
              "resuming a finished VM reports an error instead of crashing");
        CHECK(zym_resume(vm) == ZYM_STATUS_RUNTIME_ERROR,
              "and stays safe when called again");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);

        ZymVM* fresh = zym_newVM(NULL);
        CHECK(zym_resume(fresh) == ZYM_STATUS_RUNTIME_ERROR,
              "resuming a VM that never ran is an error, not a crash");
        zym_freeVM(fresh);
    }

    // ---- script entries with callbacks come back cleanly -----------------
    {
        // A rearming entry used to fire exactly once: preemptArm skips a
        // masked entry, so while the callback ran the counter was armed from
        // whatever was left (INT32_MAX with nothing else live), and nothing
        // re-armed once the callback returned and unmasked it.
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm, REARM_SRC);
        CHECK(c != NULL, "rearm fixture compiles");
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_OK, "rearm fixture runs");
        zym_call(vm, "get", 0);
        CHECK(zym_asNumber(zym_getCallResult(vm)) > 1,
              "a rearming script entry fires repeatedly, not once");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }
    {
        // A one-shot whose callback ran was left live with remaining <= 0.
        // preemptArm clamps that to 1, so it re-fired on the next instruction
        // and never stopped. It must retire instead.
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm, ONESHOT_SRC);
        CHECK(c != NULL, "oneshot fixture compiles");
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_OK,
              "a one-shot callback does not re-fire forever");
        zym_call(vm, "get", 0);
        CHECK(zym_asNumber(zym_getCallResult(vm)) == 1,
              "a one-shot callback runs exactly once");
        zym_call(vm, "rem", 0);
        CHECK(zym_asNumber(zym_getCallResult(vm)) == -1,
              "and the entry is retired afterwards");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }
    {
        // Leaving a shield unmasks maskable entries, which also needs a
        // re-arm; without it the shield retired them permanently.
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm, SHIELD_RELEASE_SRC);
        CHECK(c != NULL, "shield-release fixture compiles");
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_OK, "shield-release fixture runs");
        zym_call(vm, "afterShield", 0);
        CHECK(zym_asNumber(zym_getCallResult(vm)) > 0,
              "maskable entries fire again once the shield exits");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- freeing the chunk a suspended VM is parked in -------------------
    {
        // An abort suspends rather than unwinds, so `ip` still points into the
        // chunk. A host that frees it there -- which the CLI's ChildVM did on
        // every aborted run -- must not leave the VM resumable: dispatching from
        // released bytecode killed the process, reachable from ordinary script.
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm, SPIN_FOREVER);
        zym_preemptRegister(vm, 200000, zym_newNull(), 0);
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_ABORTED,
              "watchdog leaves the VM suspended inside the chunk");
        zym_freeChunk(vm, c);
        CHECK(zym_resume(vm) == ZYM_STATUS_RUNTIME_ERROR,
              "resume after the chunk was freed errors instead of running freed bytecode");
        CHECK(zym_resume(vm) == ZYM_STATUS_RUNTIME_ERROR,
              "and stays safe on a second attempt");
        zym_freeVM(vm);
    }

    printf(failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
