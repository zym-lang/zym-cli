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

    // ---- an unusable callback cannot be registered -----------------------
    {
        // pushPreemptFrame requires arity 0. A callback taking arguments used
        // to register fine, consume a table slot, come due every slice, and
        // never run -- with nothing reported. If registration succeeds, the
        // callback has to be invocable.
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm,
            "var fired = 0\n"
            "Preempt.every(1000, func(x) { fired = fired + 1 })\n");
        CHECK(c != NULL, "arity fixture compiles");
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_RUNTIME_ERROR,
              "registering a non-zero-arity callback is refused, not accepted");
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }
    {
        // Same guard on the host path, reported the way a full table is: id 0.
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* c = compile_or_null(vm,
            "func takesOne(a) { return a }\n"
            "func takesNone() { return 1 }\n"
            "func getOne() { return takesOne }\n"
            "func getNone() { return takesNone }\n");
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_OK, "host arity fixture runs");

        CHECK(zym_preemptRegister(vm, 1000, zym_newNull(), 0) != 0,
              "a callback-less watchdog registers");

        zym_call(vm, "getOne", 0);
        ZymValue oneArg = zym_getCallResult(vm);
        zym_pushRoot(vm, oneArg);
        CHECK(zym_preemptRegister(vm, 1000, oneArg, 0) == 0,
              "the host cannot register a non-zero-arity callback either");
        zym_popRoot(vm);

        zym_call(vm, "getNone", 0);
        ZymValue noArg = zym_getCallResult(vm);
        zym_pushRoot(vm, noArg);
        CHECK(zym_preemptRegister(vm, 1000, noArg, 0) != 0,
              "a zero-arity callback registers normally");
        zym_popRoot(vm);

        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- the host reserve ------------------------------------------------
    {
        // Script must not be able to starve the host of slots, and its own
        // budget must not move under it: whatever capacity it reads at the
        // start of a run is still bindable at the end.
        const int cap     = zym_preemptCapacity();
        const int reserve = cap / 4 > 0 ? cap / 4 : 1;

        ZymVM* vm = zym_newVM(NULL);
        CHECK(zym_getHostPreemptReserve(vm) == 0, "no reserve by default");
        CHECK(zym_setHostPreemptReserve(vm, reserve),
              "the reserve is settable before the VM has executed");
        CHECK(zym_preemptScriptCapacity(vm) == cap - reserve,
              "script capacity is capacity minus reserve");
        CHECK(!zym_setHostPreemptReserve(vm, -1) &&
              !zym_setHostPreemptReserve(vm, cap + 1),
              "out-of-range reserves are refused");

        ZymChunk* c = compile_or_null(vm,
            "var mine = []\n"
            "var capStart = Preempt.capacity()\n"
            "while (Preempt.available() > 0) {\n"
            "    push(mine, Preempt.every(900000, func() { var z = 0 }))\n"
            "}\n"
            "func held() { return length(mine) }\n"
            "func capStartWas() { return capStart }\n"
            "func capNow() { return Preempt.capacity() }\n"
            "func idsLen() { return length(Preempt.ids()) }\n"
            "func avail() { return Preempt.available() }\n");
        CHECK(c != NULL, "reserve fixture compiles");
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_OK, "reserve fixture runs");

        zym_call(vm, "held", 0);
        CHECK((int)zym_asNumber(zym_getCallResult(vm)) == cap - reserve,
              "script fills exactly its ceiling and no further");
        zym_call(vm, "capStartWas", 0);
        int cap_start = (int)zym_asNumber(zym_getCallResult(vm));
        zym_call(vm, "capNow", 0);
        CHECK(cap_start == (int)zym_asNumber(zym_getCallResult(vm)),
              "the capacity script saw at the start still holds at the end");
        zym_call(vm, "idsLen", 0);
        CHECK((int)zym_asNumber(zym_getCallResult(vm)) == cap - reserve,
              "ids() lists everything script owns");
        zym_call(vm, "avail", 0);
        CHECK((int)zym_asNumber(zym_getCallResult(vm)) == 0,
              "available() reads zero once the ceiling is reached");

        // The reserve is still the host's, after script took all it could.
        int claimed = 0;
        for (int i = 0; i < reserve; i++) {
            if (zym_preemptRegister(vm, 900000, zym_newNull(), 0) != 0) claimed++;
        }
        CHECK(claimed == reserve,
              "the host can still claim every reserved slot afterwards");
        CHECK(zym_preemptCount(vm, true) == cap - reserve, "per-owner counts are right");
        CHECK(zym_preemptCount(vm, false) == cap, "and the total is the full table");

        CHECK(!zym_setHostPreemptReserve(vm, 1),
              "the reserve is locked once the VM has executed");

        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }
    {
        // Host entries must be invisible, not merely untouchable: ids are
        // handed out sequentially, so an ungated remaining() would let script
        // map the host's supervision by probing 1, 2, 3...
        ZymVM* vm = zym_newVM(NULL);
        for (int i = 0; i < 4; i++) zym_preemptRegister(vm, 900000, zym_newNull(), 0);

        ZymChunk* c = compile_or_null(vm,
            "var mine = Preempt.every(900000, func() { var z = 0 })\n"
            "func probed() {\n"
            "    var seen = 0\n"
            "    var i = 1\n"
            "    while (i < 60) {\n"
            "        if (Preempt.remaining(i) >= 0) { seen = seen + 1 }\n"
            "        i = i + 1\n"
            "    }\n"
            "    return seen\n"
            "}\n"
            "func ownVisible() { return Preempt.remaining(mine) >= 0 }\n");
        CHECK(zym_runChunk(vm, c) == ZYM_STATUS_OK, "probe fixture runs");

        zym_call(vm, "probed", 0);
        CHECK((int)zym_asNumber(zym_getCallResult(vm)) == 1,
              "probing the id space finds only script's own entry");
        zym_call(vm, "ownVisible", 0);
        CHECK(zym_asBool(zym_getCallResult(vm)),
              "while its own entry stays readable");
        CHECK(zym_preemptRemaining(vm, 1) >= 0,
              "the host can still read a host entry it owns");

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
