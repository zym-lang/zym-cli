// A continuation's resume target must never outlive the bytecode it points at.
//
// ObjContinuation stores saved_ip/saved_chunk -- a raw position no frame covers.
// A Chunk is not a GC object: it is either embedded by value in an ObjFunction
// and swept with it, or standalone and released by the host via zym_freeChunk.
// Neither is protected by marking saved_chunk, because a Chunk carries no
// back-pointer to its owner and markChunk only walks constants.
//
// Two defences, one per ownership model, and this file tests both:
//   - function-owned: the continuation records saved_owner at capture and the
//     GC marks it, so the bytecode cannot be collected out from under it
//   - host-owned: zym_freeChunk retires every continuation aiming into the
//     chunk it is about to release (CONT_INVALID), so a later resume reports
//     an error instead of dispatching freed memory
//
// The host must survive every case here. A crash is a failure, not a pass.

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

// Chunk A owns the capture. `grab` parks the continuation in a map so the host
// can fetch it with `get` -- a capture hands its continuation to the prompt's
// frame, so it cannot simply be assigned inside the capturing function.
// saved_chunk then points at inner's bytecode, which chunk A's constant pool
// owns, so freeing chunk A is what puts the resume target out of reach.
static const char* CHUNK_A =
    "var TAG = Cont.newPrompt(\"t\")\n"
    "var SLOT = { k: null }\n"
    "func inner() {\n"
    "    Cont.capture(TAG)\n"
    "    return 1\n"
    "}\n"
    "func grab() {\n"
    "    Cont.pushPrompt(TAG)\n"
    "    var r = inner()\n"
    "    if (Cont.isContinuation(r)) { SLOT.k = r }\n"
    "    return 0\n"
    "}\n"
    "func get() { return SLOT.k }\n";

// The case no marking can save. Capturing at the top level of a chunk, with the
// prompt pushed at the same depth, gives a zero-frame capture whose saved_chunk
// is the host-owned chunk itself -- there is no ObjFunction to record as owner,
// so saved_owner is NULL and the GC has nothing it could pin. Only
// zym_freeChunk retiring the continuation stands between a later resume and
// released bytecode.
static const char* CHUNK_HOST =
    "var TAG2 = Cont.newPrompt(\"h\")\n"
    "var HOLD = { k: null }\n"
    "Cont.pushPrompt(TAG2)\n"
    "var z = Cont.capture(TAG2)\n"
    "if (Cont.isContinuation(z)) { HOLD.k = z }\n"
    "func getHost() { return HOLD.k }\n";

// Chunk B lives in its own allocation, so it survives chunk A being freed and
// can still be called to interrogate the continuation afterwards.
static const char* CHUNK_B =
    "func isLive(k) { return Cont.isValid(k) }\n"
    "func isCont(k) { return Cont.isContinuation(k) }\n";

// Allocation pressure, to make the collector run while the continuation is the
// only thing referencing its resume target.
static const char* CHURN =
    "func churn() {\n"
    "    var out = []\n"
    "    var i = 0\n"
    "    while (i < 4000) { push(out, str(i))\n i = i + 1 }\n"
    "    return length(out)\n"
    "}\n";

static ZymChunk* compile_or_null(ZymVM* vm, const char* src) {
    ZymCompilerConfig cfg = { 1 };
    ZymChunk* chunk = zym_newChunk(vm);
    if (zym_compile(vm, src, chunk, NULL, "t.zym", cfg, NULL) != ZYM_STATUS_OK) {
        zym_freeChunk(vm, chunk);
        return NULL;
    }
    return chunk;
}

static bool call_pred(ZymVM* vm, const char* fn, ZymValue arg) {
    if (zym_callv(vm, fn, 1, &arg) != ZYM_STATUS_OK) return false;
    ZymValue r = zym_getCallResult(vm);
    return zym_isBool(r) && zym_asBool(r);
}

int main(void) {
    // ---- host-owned chunk freed under a live continuation ---------------
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* a = compile_or_null(vm, CHUNK_A);
        ZymChunk* b = compile_or_null(vm, CHUNK_B);
        CHECK(a != NULL && b != NULL, "fixtures compile");
        CHECK(zym_runChunk(vm, a) == ZYM_STATUS_OK, "chunk A runs");
        CHECK(zym_runChunk(vm, b) == ZYM_STATUS_OK, "chunk B runs");

        CHECK(zym_callv(vm, "grab", 0, NULL) == ZYM_STATUS_OK, "grab() returns");
        CHECK(zym_callv(vm, "get", 0, NULL) == ZYM_STATUS_OK, "get() returns");
        ZymValue k = zym_getCallResult(vm);
        zym_pushRoot(vm, k);   // hold it across the free and any collection
        CHECK(call_pred(vm, "isCont", k), "the capture produced a continuation");
        CHECK(call_pred(vm, "isLive", k), "it is valid before the chunk is freed");

        // saved_chunk points into chunk A, which the host now releases.
        zym_freeChunk(vm, a);

        CHECK(call_pred(vm, "isCont", k),
              "the continuation object itself survives the free");
        CHECK(!call_pred(vm, "isLive", k),
              "it was retired when its chunk was freed");

        zym_popRoot(vm);
        zym_freeChunk(vm, b);
        zym_freeVM(vm);
    }

    // ---- resuming a retired continuation is an error, not a crash -------
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* a = compile_or_null(vm, CHUNK_A);
        ZymChunk* b = compile_or_null(vm,
            "func kick(k) { Cont.pushPrompt(Cont.newPrompt(\"z\"))\n"
            "               return Cont.resume(k, 5) }\n");
        CHECK(a != NULL && b != NULL, "fixtures compile");
        zym_runChunk(vm, a);
        zym_runChunk(vm, b);

        zym_callv(vm, "grab", 0, NULL);
        zym_callv(vm, "get", 0, NULL);
        ZymValue k = zym_getCallResult(vm);
        zym_pushRoot(vm, k);

        zym_freeChunk(vm, a);

        // Must be refused by the state guard rather than jumping into the
        // released bytecode.
        ZymStatus st = zym_callv(vm, "kick", 1, &k);
        CHECK(st != ZYM_STATUS_OK,
              "resuming into a freed chunk is refused instead of dispatching it");

        zym_popRoot(vm);
        zym_freeChunk(vm, b);
        zym_freeVM(vm);
    }

    // ---- function-owned target survives collection ----------------------
    {
        // Here the chunk is never freed by the host, so the only thing that
        // could reclaim the resume target is the collector. saved_owner is what
        // stops it.
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* a = compile_or_null(vm, CHUNK_A);
        ZymChunk* b = compile_or_null(vm, CHUNK_B);
        ZymChunk* c = compile_or_null(vm, CHURN);
        CHECK(a && b && c, "fixtures compile");
        zym_runChunk(vm, a);
        zym_runChunk(vm, b);
        zym_runChunk(vm, c);

        zym_callv(vm, "grab", 0, NULL);
        zym_callv(vm, "get", 0, NULL);
        ZymValue k = zym_getCallResult(vm);
        zym_pushRoot(vm, k);

        for (int i = 0; i < 5; i++) {
            CHECK(zym_callv(vm, "churn", 0, NULL) == ZYM_STATUS_OK,
                  "allocation churn runs with a live continuation");
        }

        CHECK(call_pred(vm, "isLive", k),
              "the continuation is still valid after repeated collection");

        zym_popRoot(vm);
        zym_freeChunk(vm, a);
        zym_freeChunk(vm, b);
        zym_freeChunk(vm, c);
        zym_freeVM(vm);
    }

    // ---- freeing an unrelated chunk must not retire anything ------------
    {
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* a = compile_or_null(vm, CHUNK_A);
        ZymChunk* b = compile_or_null(vm, CHUNK_B);
        ZymChunk* spare = compile_or_null(vm, "func unused() { return 0 }\n");
        CHECK(a && b && spare, "fixtures compile");
        zym_runChunk(vm, a);
        zym_runChunk(vm, b);
        zym_runChunk(vm, spare);

        zym_callv(vm, "grab", 0, NULL);
        zym_callv(vm, "get", 0, NULL);
        ZymValue k = zym_getCallResult(vm);
        zym_pushRoot(vm, k);

        zym_freeChunk(vm, spare);   // nothing to do with this continuation
        CHECK(call_pred(vm, "isLive", k),
              "freeing an unrelated chunk leaves the continuation alone");

        zym_popRoot(vm);
        zym_freeChunk(vm, a);
        zym_freeChunk(vm, b);
        zym_freeVM(vm);
    }

    // ---- the unprotectable case: target IS the host-owned chunk ----------
    {
        // saved_owner is NULL here, so nothing the GC does can keep the resume
        // target alive. Without zym_freeChunk retiring the continuation, a
        // resume dispatches straight into released bytecode.
        ZymVM* vm = zym_newVM(NULL);
        ZymChunk* host = compile_or_null(vm, CHUNK_HOST);
        ZymChunk* b = compile_or_null(vm, CHUNK_B);
        ZymChunk* kicker = compile_or_null(vm,
            "func kick2(k) { Cont.pushPrompt(Cont.newPrompt(\"z\"))\n"
            "                return Cont.resume(k, 5) }\n");
        CHECK(host && b && kicker, "fixtures compile");
        CHECK(zym_runChunk(vm, host) == ZYM_STATUS_OK, "top-level capture runs");
        zym_runChunk(vm, b);
        zym_runChunk(vm, kicker);

        CHECK(zym_callv(vm, "getHost", 0, NULL) == ZYM_STATUS_OK, "getHost() returns");
        ZymValue k = zym_getCallResult(vm);
        zym_pushRoot(vm, k);
        CHECK(call_pred(vm, "isCont", k), "top-level capture yielded a continuation");
        CHECK(call_pred(vm, "isLive", k), "valid while its chunk is alive");

        zym_freeChunk(vm, host);

        CHECK(!call_pred(vm, "isLive", k),
              "retired when the host-owned chunk it targets was freed");
        CHECK(zym_callv(vm, "kick2", 1, &k) != ZYM_STATUS_OK,
              "resuming into the freed host chunk is refused, not dispatched");

        zym_popRoot(vm);
        zym_freeChunk(vm, b);
        zym_freeChunk(vm, kicker);
        zym_freeVM(vm);
    }

    printf(failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
