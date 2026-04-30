// Godot-backed Hash native (`HashingContext`).
//
// Streaming non-keyed hashes (MD5, SHA-1, SHA-256). For keyed/HMAC
// digests, RSA signing, or random bytes, use the `Crypto` native.
//
// Surface (statics):
//   Hash.create(algo)               -> Hash instance, ready to feed
//   Hash.digest(algo, buf)          -> Buffer (one-shot convenience)
//
// Instance:
//   update(buf)                     -> bool   (true on success)
//   finish()                        -> Buffer (digest bytes; finalises)
//   reset()                         -> bool   (start a fresh round of the
//                                              same algorithm; instance is
//                                              reusable after this)
//
// `algo` is one of `"md5"`, `"sha1"`, `"sha256"` (case-insensitive). After
// calling `finish()` the instance must be `reset()` before the next
// `update()`/`finish()` pair; calling `update()` on a finished context
// returns `false`. Inputs and outputs are zym Buffers, so this composes
// directly with `File.readAllBytes`, `Process.exec`, etc.
#include "core/crypto/hashing_context.h"
#include "core/error/error_list.h"
#include "core/string/ustring.h"

#include "natives.hpp"

// Provided by buffer.cpp.
extern ZymValue makeBufferInstance(ZymVM* vm, const PackedByteArray& src);

// ---- handle ----

struct HashHandle {
    Ref<HashingContext> h;
    HashingContext::HashType type = HashingContext::HASH_SHA256;
    bool started = false;
    bool finished = false;
};

static void hashFinalizer(ZymVM*, void* data) { delete static_cast<HashHandle*>(data); }

static HashHandle* unwrapHash(ZymValue ctx) { return static_cast<HashHandle*>(zym_getNativeData(ctx)); }

// ---- helpers ----

static bool reqStr(ZymVM* vm, ZymValue v, const char* where, String* out) {
    if (!zym_isString(v)) { zym_runtimeError(vm, "%s expects a string", where); return false; }
    *out = String::utf8(zym_asCString(v)); return true;
}

static bool reqHashType(ZymVM* vm, ZymValue v, const char* where, HashingContext::HashType* out) {
    String s; if (!reqStr(vm, v, where, &s)) return false;
    String l = s.to_lower();
    if (l == "md5")    { *out = HashingContext::HASH_MD5;    return true; }
    if (l == "sha1")   { *out = HashingContext::HASH_SHA1;   return true; }
    if (l == "sha256") { *out = HashingContext::HASH_SHA256; return true; }
    zym_runtimeError(vm, "%s: unknown hash type '%s' (expected 'md5', 'sha1', or 'sha256')", where, s.utf8().get_data());
    return false;
}

static bool reqBuffer(ZymVM* vm, ZymValue v, const char* where, PackedByteArray** out) {
    if (zym_isMap(v)) {
        ZymValue ctx = zym_mapGet(vm, v, "__pba__");
        if (ctx != ZYM_ERROR) {
            void* data = zym_getNativeData(ctx);
            if (data) { *out = static_cast<PackedByteArray*>(data); return true; }
        }
    }
    zym_runtimeError(vm, "%s expects a Buffer", where);
    return false;
}

// ---- forward decl ----

static ZymValue makeHashInstance(ZymVM* vm, Ref<HashingContext> h, HashingContext::HashType type);

// ---- instance methods ----

static ZymValue h_update(ZymVM* vm, ZymValue ctx, ZymValue bufV) {
    HashHandle* hh = unwrapHash(ctx);
    if (!hh || hh->h.is_null()) { zym_runtimeError(vm, "Hash.update: invalid handle"); return ZYM_ERROR; }
    if (hh->finished) return zym_newBool(false);
    PackedByteArray* buf; if (!reqBuffer(vm, bufV, "Hash.update(buf)", &buf)) return ZYM_ERROR;
    // Engine rejects empty input; treat zero-byte chunks as a successful
    // no-op so callers can feed arbitrary slices without special-casing.
    if (buf->size() == 0) return zym_newBool(true);
    return zym_newBool(hh->h->update(*buf) == OK);
}

static ZymValue h_finish(ZymVM* vm, ZymValue ctx) {
    HashHandle* hh = unwrapHash(ctx);
    if (!hh || hh->h.is_null()) { zym_runtimeError(vm, "Hash.finish: invalid handle"); return ZYM_ERROR; }
    if (hh->finished) return makeBufferInstance(vm, PackedByteArray());
    PackedByteArray digest = hh->h->finish();
    hh->finished = true;
    return makeBufferInstance(vm, digest);
}

static ZymValue h_reset(ZymVM* vm, ZymValue ctx) {
    HashHandle* hh = unwrapHash(ctx);
    if (!hh || hh->h.is_null()) { zym_runtimeError(vm, "Hash.reset: invalid handle"); return ZYM_ERROR; }
    Error err = hh->h->start(hh->type);
    hh->started = (err == OK);
    hh->finished = false;
    return zym_newBool(err == OK);
}

// ---- instance assembly ----

static ZymValue makeHashInstance(ZymVM* vm, Ref<HashingContext> h, HashingContext::HashType type) {
    auto* data = new HashHandle{ h, type, true, false };
    ZymValue ctxv = zym_createNativeContext(vm, data, hashFinalizer);
    zym_pushRoot(vm, ctxv);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__hash__", ctxv);

#define M(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctxv); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)

    M("update", "update(buf)", h_update);
    M("finish", "finish()",    h_finish);
    M("reset",  "reset()",     h_reset);

#undef M

    zym_popRoot(vm);
    zym_popRoot(vm);
    return obj;
}

// ---- Hash global (statics) ----

static ZymValue f_create(ZymVM* vm, ZymValue, ZymValue algoV) {
    HashingContext::HashType type;
    if (!reqHashType(vm, algoV, "Hash.create(algo)", &type)) return ZYM_ERROR;
    Ref<HashingContext> h;
    h.instantiate();
    if (h->start(type) != OK) {
        zym_runtimeError(vm, "Hash.create(algo): failed to initialize");
        return ZYM_ERROR;
    }
    return makeHashInstance(vm, h, type);
}

static ZymValue f_digest(ZymVM* vm, ZymValue, ZymValue algoV, ZymValue bufV) {
    HashingContext::HashType type;
    if (!reqHashType(vm, algoV, "Hash.digest(algo, buf)", &type)) return ZYM_ERROR;
    PackedByteArray* buf; if (!reqBuffer(vm, bufV, "Hash.digest(algo, buf)", &buf)) return ZYM_ERROR;
    Ref<HashingContext> h;
    h.instantiate();
    if (h->start(type) != OK) { zym_runtimeError(vm, "Hash.digest(algo, buf): failed to initialize"); return ZYM_ERROR; }
    // Engine's HashingContext::update rejects empty input; skip it so the
    // empty-string digest is still produced correctly.
    if (buf->size() > 0 && h->update(*buf) != OK) {
        zym_runtimeError(vm, "Hash.digest(algo, buf): update failed");
        return ZYM_ERROR;
    }
    return makeBufferInstance(vm, h->finish());
}

// ---- factory ----

ZymValue nativeHash_create(ZymVM* vm) {
    ZymValue ctxv = zym_createNativeContext(vm, nullptr, nullptr);
    zym_pushRoot(vm, ctxv);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

#define F(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctxv); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)

    F("create", "create(algo)",      f_create);
    F("digest", "digest(algo, buf)", f_digest);

#undef F

    zym_popRoot(vm);
    zym_popRoot(vm);
    return obj;
}
