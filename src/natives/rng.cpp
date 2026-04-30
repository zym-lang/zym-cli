// Godot-backed Random native (`RandomNumberGenerator`).
//
// Surface:
//   Random.create()                 -> RNG instance, randomized seed
//   Random.create(seed)             -> RNG instance, deterministic seed
//
// Instance:
//   seed(s)                         -> null   (alias of setSeed)
//   getSeed()                       -> number
//   setSeed(s)                      -> null
//   getState()                      -> number
//   setState(s)                     -> null
//   randomize()                     -> null   (re-seeds from system entropy)
//   randi()                         -> number (uint32 as double)
//   randf()                         -> number in [0, 1)
//   randfRange(lo, hi)              -> number in [lo, hi]
//   randfn()                        -> number, normal(0, 1)
//   randfn(mean, deviation)         -> number, normal(mean, deviation)
//   randiRange(lo, hi)              -> integer in [lo, hi] (inclusive)
//   randWeighted(weights)           -> integer index (0..len-1) per weights
//
// Notes:
// - The RNG is a non-crypto PCG. For cryptographic randomness use
//   `Crypto.generateRandomBytes(...)`.
// - Seeds and states are unsigned 64-bit integers. zym numbers are 64-bit
//   doubles, so values above 2^53 will lose precision when read back. For
//   reproducibility prefer seeds <= 2^53.
// - `randWeighted` accepts a list of non-negative numbers; an empty or
//   all-zero list yields -1 (matches Godot's behaviour).
#include "core/math/random_number_generator.h"
#include "core/templates/vector.h"

#include "natives.hpp"

// ---- handle ----

struct RngHandle { Ref<RandomNumberGenerator> r; };

static void rngFinalizer(ZymVM*, void* data) { delete static_cast<RngHandle*>(data); }

static RngHandle* unwrapRng(ZymValue ctx) { return static_cast<RngHandle*>(zym_getNativeData(ctx)); }

// ---- helpers ----

static bool reqNum(ZymVM* vm, ZymValue v, const char* where, double* out) {
    if (!zym_isNumber(v)) { zym_runtimeError(vm, "%s expects a number", where); return false; }
    *out = zym_asNumber(v); return true;
}
static bool reqInt(ZymVM* vm, ZymValue v, const char* where, int64_t* out) {
    if (!zym_isNumber(v)) { zym_runtimeError(vm, "%s expects a number", where); return false; }
    *out = (int64_t)zym_asNumber(v); return true;
}
static bool reqU64(ZymVM* vm, ZymValue v, const char* where, uint64_t* out) {
    if (!zym_isNumber(v)) { zym_runtimeError(vm, "%s expects a number", where); return false; }
    double d = zym_asNumber(v);
    if (d < 0) d = 0;
    *out = (uint64_t)d; return true;
}

// ---- forward decl ----

static ZymValue makeRngInstance(ZymVM* vm, Ref<RandomNumberGenerator> r);

// ---- instance methods ----

static ZymValue r_setSeed(ZymVM* vm, ZymValue ctx, ZymValue sV) {
    RngHandle* h = unwrapRng(ctx);
    if (!h || h->r.is_null()) { zym_runtimeError(vm, "Random.setSeed: invalid handle"); return ZYM_ERROR; }
    uint64_t s; if (!reqU64(vm, sV, "Random.setSeed(seed)", &s)) return ZYM_ERROR;
    h->r->set_seed(s);
    return zym_newNull();
}

static ZymValue r_getSeed(ZymVM* vm, ZymValue ctx) {
    RngHandle* h = unwrapRng(ctx);
    if (!h || h->r.is_null()) { zym_runtimeError(vm, "Random.getSeed: invalid handle"); return ZYM_ERROR; }
    return zym_newNumber((double)h->r->get_seed());
}

static ZymValue r_setState(ZymVM* vm, ZymValue ctx, ZymValue sV) {
    RngHandle* h = unwrapRng(ctx);
    if (!h || h->r.is_null()) { zym_runtimeError(vm, "Random.setState: invalid handle"); return ZYM_ERROR; }
    uint64_t s; if (!reqU64(vm, sV, "Random.setState(state)", &s)) return ZYM_ERROR;
    h->r->set_state(s);
    return zym_newNull();
}

static ZymValue r_getState(ZymVM* vm, ZymValue ctx) {
    RngHandle* h = unwrapRng(ctx);
    if (!h || h->r.is_null()) { zym_runtimeError(vm, "Random.getState: invalid handle"); return ZYM_ERROR; }
    return zym_newNumber((double)h->r->get_state());
}

static ZymValue r_randomize(ZymVM* vm, ZymValue ctx) {
    RngHandle* h = unwrapRng(ctx);
    if (!h || h->r.is_null()) { zym_runtimeError(vm, "Random.randomize: invalid handle"); return ZYM_ERROR; }
    h->r->randomize();
    return zym_newNull();
}

static ZymValue r_randi(ZymVM* vm, ZymValue ctx) {
    RngHandle* h = unwrapRng(ctx);
    if (!h || h->r.is_null()) { zym_runtimeError(vm, "Random.randi: invalid handle"); return ZYM_ERROR; }
    return zym_newNumber((double)(uint32_t)h->r->randi());
}

static ZymValue r_randf(ZymVM* vm, ZymValue ctx) {
    RngHandle* h = unwrapRng(ctx);
    if (!h || h->r.is_null()) { zym_runtimeError(vm, "Random.randf: invalid handle"); return ZYM_ERROR; }
    return zym_newNumber((double)h->r->randf());
}

static ZymValue r_randfRange(ZymVM* vm, ZymValue ctx, ZymValue loV, ZymValue hiV) {
    RngHandle* h = unwrapRng(ctx);
    if (!h || h->r.is_null()) { zym_runtimeError(vm, "Random.randfRange: invalid handle"); return ZYM_ERROR; }
    double lo, hi;
    if (!reqNum(vm, loV, "Random.randfRange(lo, hi)", &lo)) return ZYM_ERROR;
    if (!reqNum(vm, hiV, "Random.randfRange(lo, hi)", &hi)) return ZYM_ERROR;
    return zym_newNumber((double)h->r->randf_range((real_t)lo, (real_t)hi));
}

static ZymValue r_randfn(ZymVM* vm, ZymValue ctx, ZymValue* vargs, int vargc) {
    RngHandle* h = unwrapRng(ctx);
    if (!h || h->r.is_null()) { zym_runtimeError(vm, "Random.randfn: invalid handle"); return ZYM_ERROR; }
    double mean = 0.0, dev = 1.0;
    if (vargc > 0) { if (!reqNum(vm, vargs[0], "Random.randfn(mean, deviation)", &mean)) return ZYM_ERROR; }
    if (vargc > 1) { if (!reqNum(vm, vargs[1], "Random.randfn(mean, deviation)", &dev))  return ZYM_ERROR; }
    return zym_newNumber((double)h->r->randfn((real_t)mean, (real_t)dev));
}

static ZymValue r_randiRange(ZymVM* vm, ZymValue ctx, ZymValue loV, ZymValue hiV) {
    RngHandle* h = unwrapRng(ctx);
    if (!h || h->r.is_null()) { zym_runtimeError(vm, "Random.randiRange: invalid handle"); return ZYM_ERROR; }
    int64_t lo, hi;
    if (!reqInt(vm, loV, "Random.randiRange(lo, hi)", &lo)) return ZYM_ERROR;
    if (!reqInt(vm, hiV, "Random.randiRange(lo, hi)", &hi)) return ZYM_ERROR;
    return zym_newNumber((double)h->r->randi_range((int)lo, (int)hi));
}

static ZymValue r_randWeighted(ZymVM* vm, ZymValue ctx, ZymValue weightsV) {
    RngHandle* h = unwrapRng(ctx);
    if (!h || h->r.is_null()) { zym_runtimeError(vm, "Random.randWeighted: invalid handle"); return ZYM_ERROR; }
    if (!zym_isList(weightsV)) {
        zym_runtimeError(vm, "Random.randWeighted(weights): expects a list of numbers");
        return ZYM_ERROR;
    }
    int n = zym_listLength(weightsV);
    Vector<float> weights;
    weights.resize(n);
    for (int i = 0; i < n; i++) {
        ZymValue elem = zym_listGet(vm, weightsV, i);
        if (!zym_isNumber(elem)) {
            zym_runtimeError(vm, "Random.randWeighted(weights): element %d is not a number", i);
            return ZYM_ERROR;
        }
        weights.write[i] = (float)zym_asNumber(elem);
    }
    return zym_newNumber((double)h->r->rand_weighted(weights));
}

// ---- instance assembly ----

static ZymValue makeRngInstance(ZymVM* vm, Ref<RandomNumberGenerator> r) {
    auto* data = new RngHandle{ r };
    ZymValue ctxv = zym_createNativeContext(vm, data, rngFinalizer);
    zym_pushRoot(vm, ctxv);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__rng__", ctxv);

#define M(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctxv); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)
#define MV(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosureVariadic(vm, sig, (void*)fn, ctxv); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)

    M("seed",          "seed(s)",                   r_setSeed);
    M("setSeed",       "setSeed(s)",                r_setSeed);
    M("getSeed",       "getSeed()",                 r_getSeed);
    M("setState",      "setState(s)",               r_setState);
    M("getState",      "getState()",                r_getState);
    M("randomize",     "randomize()",               r_randomize);
    M("randi",         "randi()",                   r_randi);
    M("randf",         "randf()",                   r_randf);
    M("randfRange",    "randfRange(lo, hi)",        r_randfRange);
    MV("randfn",       "randfn(...)",               r_randfn);
    M("randiRange",    "randiRange(lo, hi)",        r_randiRange);
    M("randWeighted",  "randWeighted(weights)",     r_randWeighted);

#undef M
#undef MV

    zym_popRoot(vm);
    zym_popRoot(vm);
    return obj;
}

// ---- Random global (statics) ----

static ZymValue f_create(ZymVM* vm, ZymValue, ZymValue* vargs, int vargc) {
    Ref<RandomNumberGenerator> r;
    r.instantiate();
    if (vargc > 0) {
        uint64_t s; if (!reqU64(vm, vargs[0], "Random.create(seed)", &s)) return ZYM_ERROR;
        r->set_seed(s);
    }
    return makeRngInstance(vm, r);
}

// ---- factory ----

ZymValue nativeRandom_create(ZymVM* vm) {
    ZymValue ctxv = zym_createNativeContext(vm, nullptr, nullptr);
    zym_pushRoot(vm, ctxv);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

    {
        ZymValue cl = zym_createNativeClosureVariadic(vm, "create(...)", (void*)f_create, ctxv);
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, "create", cl); zym_popRoot(vm);
    }

    zym_popRoot(vm);
    zym_popRoot(vm);
    return obj;
}
