// Godot-backed AES native (`AESContext`).
//
// Symmetric block cipher: AES-128 or AES-256 in ECB or CBC mode (AES-192
// is not supported by the underlying cipher implementation).
// The convenience helpers (`encryptCbc` / `decryptCbc`) handle PKCS#7
// padding so callers can pass arbitrary-length plaintext; the instance
// API is a direct mirror of the engine's `AESContext` and requires the
// caller to feed full 16-byte blocks.
//
// Surface (statics):
//   AES.create()                            -> AES instance
//   AES.encryptCbc(key, iv, plaintextBuf)   -> Buffer  (PKCS#7 padded)
//   AES.decryptCbc(key, iv, ciphertextBuf)  -> Buffer | null  (null on bad padding / not 16-aligned)
//
// Instance:
//   start(mode, key [, iv])                 -> "ok" | runtime error
//   update(buf)                             -> Buffer (must be a multiple of 16 bytes)
//   ivState()                               -> Buffer  (CBC modes only; current chaining state)
//   finish()                                -> "ok"
//
// `mode` is one of `"ecb-encrypt"`, `"ecb-decrypt"`, `"cbc-encrypt"`,
// `"cbc-decrypt"` (case-insensitive). Keys must be 16 or 32 bytes; CBC
// modes require a 16-byte `iv`. ECB is exposed for completeness but is
// rarely the right choice — see `docs/aes.md`.
//
// AES-CBC is **unauthenticated**: a flipped ciphertext bit produces
// garbage plaintext rather than an error. Pair it with `Crypto.hmacDigest`
// (encrypt-then-MAC) for integrity, or use a future GCM helper when one
// lands.
#include "core/crypto/aes_context.h"
#include "core/error/error_list.h"
#include "core/string/ustring.h"

#include "natives.hpp"

// Provided by buffer.cpp.
extern ZymValue makeBufferInstance(ZymVM* vm, const PackedByteArray& src);

// ---- handle ----

struct AesHandle {
    Ref<AESContext> ctx;
    AESContext::Mode mode = AESContext::MODE_MAX;
    bool started = false;
};

static void aesFinalizer(ZymVM*, void* data) { delete static_cast<AesHandle*>(data); }

static AesHandle* unwrapAes(ZymValue ctx) { return static_cast<AesHandle*>(zym_getNativeData(ctx)); }

// ---- helpers ----

static bool reqStr(ZymVM* vm, ZymValue v, const char* where, String* out) {
    if (!zym_isString(v)) { zym_runtimeError(vm, "%s expects a string", where); return false; }
    *out = String::utf8(zym_asCString(v)); return true;
}

static bool reqMode(ZymVM* vm, ZymValue v, const char* where, AESContext::Mode* out) {
    String s; if (!reqStr(vm, v, where, &s)) return false;
    String l = s.to_lower();
    if (l == "ecb-encrypt") { *out = AESContext::MODE_ECB_ENCRYPT; return true; }
    if (l == "ecb-decrypt") { *out = AESContext::MODE_ECB_DECRYPT; return true; }
    if (l == "cbc-encrypt") { *out = AESContext::MODE_CBC_ENCRYPT; return true; }
    if (l == "cbc-decrypt") { *out = AESContext::MODE_CBC_DECRYPT; return true; }
    zym_runtimeError(vm, "%s: unknown mode '%s' (expected 'ecb-encrypt', 'ecb-decrypt', 'cbc-encrypt', or 'cbc-decrypt')", where, s.utf8().get_data());
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

static bool checkKeySize(ZymVM* vm, const PackedByteArray& key, const char* where) {
    int n = key.size();
    if (n != 16 && n != 32) {
        zym_runtimeError(vm, "%s: key must be 16 or 32 bytes (got %d)", where, n);
        return false;
    }
    return true;
}

static bool checkIvSize(ZymVM* vm, const PackedByteArray& iv, const char* where) {
    if (iv.size() != 16) {
        zym_runtimeError(vm, "%s: iv must be 16 bytes (got %d)", where, iv.size());
        return false;
    }
    return true;
}

// PKCS#7: pad to next 16-byte boundary; if input is already aligned, append a full 16-byte block.
static PackedByteArray pkcs7Pad(const PackedByteArray& src) {
    int padLen = 16 - (src.size() % 16);
    PackedByteArray out;
    out.resize(src.size() + padLen);
    if (src.size() > 0) {
        memcpy(out.ptrw(), src.ptr(), src.size());
    }
    uint8_t* w = out.ptrw() + src.size();
    for (int i = 0; i < padLen; ++i) w[i] = (uint8_t)padLen;
    return out;
}

// Returns true on success, false on malformed padding.
static bool pkcs7Unpad(const PackedByteArray& src, PackedByteArray* out) {
    if (src.size() == 0 || (src.size() % 16) != 0) return false;
    uint8_t pad = src.ptr()[src.size() - 1];
    if (pad == 0 || pad > 16) return false;
    if ((int)pad > src.size()) return false;
    for (int i = 0; i < pad; ++i) {
        if (src.ptr()[src.size() - 1 - i] != pad) return false;
    }
    out->resize(src.size() - pad);
    if (out->size() > 0) memcpy(out->ptrw(), src.ptr(), out->size());
    return true;
}

// ---- forward decl ----

static ZymValue makeAesInstance(ZymVM* vm, Ref<AESContext> ctx);

// ---- instance methods ----

static ZymValue a_start(ZymVM* vm, ZymValue ctxv, ZymValue* args, int argc) {
    AesHandle* hh = unwrapAes(ctxv);
    if (!hh || hh->ctx.is_null()) { zym_runtimeError(vm, "AES.start: invalid handle"); return ZYM_ERROR; }
    if (argc < 2 || argc > 3) {
        zym_runtimeError(vm, "AES.start(mode, key, iv?): expected 2 or 3 arguments, got %d", argc);
        return ZYM_ERROR;
    }
    AESContext::Mode mode;
    if (!reqMode(vm, args[0], "AES.start(mode, key, iv?)", &mode)) return ZYM_ERROR;
    PackedByteArray* key;
    if (!reqBuffer(vm, args[1], "AES.start(mode, key, iv?)", &key)) return ZYM_ERROR;
    if (!checkKeySize(vm, *key, "AES.start(mode, key, iv?)")) return ZYM_ERROR;

    PackedByteArray iv;
    bool isCbc = (mode == AESContext::MODE_CBC_ENCRYPT || mode == AESContext::MODE_CBC_DECRYPT);
    if (argc == 3 && !zym_isNull(args[2])) {
        PackedByteArray* ivBuf;
        if (!reqBuffer(vm, args[2], "AES.start(mode, key, iv?)", &ivBuf)) return ZYM_ERROR;
        if (!checkIvSize(vm, *ivBuf, "AES.start(mode, key, iv?)")) return ZYM_ERROR;
        iv = *ivBuf;
    } else if (isCbc) {
        zym_runtimeError(vm, "AES.start(mode, key, iv?): CBC mode requires a 16-byte iv");
        return ZYM_ERROR;
    }

    Error err = hh->ctx->start(mode, *key, iv);
    if (err != OK) {
        zym_runtimeError(vm, "AES.start: failed to initialize");
        return ZYM_ERROR;
    }
    hh->mode = mode;
    hh->started = true;
    return zym_newString(vm, "ok");
}

static ZymValue a_update(ZymVM* vm, ZymValue ctxv, ZymValue bufV) {
    AesHandle* hh = unwrapAes(ctxv);
    if (!hh || hh->ctx.is_null()) { zym_runtimeError(vm, "AES.update: invalid handle"); return ZYM_ERROR; }
    if (!hh->started) { zym_runtimeError(vm, "AES.update: call start(...) first"); return ZYM_ERROR; }
    PackedByteArray* buf;
    if (!reqBuffer(vm, bufV, "AES.update(buf)", &buf)) return ZYM_ERROR;
    if ((buf->size() % 16) != 0) {
        zym_runtimeError(vm, "AES.update(buf): input length must be a multiple of 16 bytes (got %d)", buf->size());
        return ZYM_ERROR;
    }
    PackedByteArray out = hh->ctx->update(*buf);
    return makeBufferInstance(vm, out);
}

static ZymValue a_ivState(ZymVM* vm, ZymValue ctxv) {
    AesHandle* hh = unwrapAes(ctxv);
    if (!hh || hh->ctx.is_null()) { zym_runtimeError(vm, "AES.ivState: invalid handle"); return ZYM_ERROR; }
    if (!hh->started) { zym_runtimeError(vm, "AES.ivState: call start(...) first"); return ZYM_ERROR; }
    bool isCbc = (hh->mode == AESContext::MODE_CBC_ENCRYPT || hh->mode == AESContext::MODE_CBC_DECRYPT);
    if (!isCbc) {
        zym_runtimeError(vm, "AES.ivState: only valid in CBC modes");
        return ZYM_ERROR;
    }
    return makeBufferInstance(vm, hh->ctx->get_iv_state());
}

static ZymValue a_finish(ZymVM* vm, ZymValue ctxv) {
    AesHandle* hh = unwrapAes(ctxv);
    if (!hh || hh->ctx.is_null()) { zym_runtimeError(vm, "AES.finish: invalid handle"); return ZYM_ERROR; }
    hh->ctx->finish();
    hh->started = false;
    return zym_newString(vm, "ok");
}

// ---- instance assembly ----

static ZymValue makeAesInstance(ZymVM* vm, Ref<AESContext> ctx) {
    auto* data = new AesHandle{ ctx, AESContext::MODE_MAX, false };
    ZymValue ctxv = zym_createNativeContext(vm, data, aesFinalizer);
    zym_pushRoot(vm, ctxv);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__aes__", ctxv);

#define M(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctxv); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)
#define MV(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosureVariadic(vm, sig, (void*)fn, ctxv); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)

    MV("start",   "start(...)",            a_start);
    M("update",   "update(buf)",           a_update);
    M("ivState",  "ivState()",             a_ivState);
    M("finish",   "finish()",              a_finish);

#undef M
#undef MV

    zym_popRoot(vm);
    zym_popRoot(vm);
    return obj;
}

// ---- AES global (statics) ----

static ZymValue f_create(ZymVM* vm, ZymValue) {
    Ref<AESContext> c;
    c.instantiate();
    return makeAesInstance(vm, c);
}

static ZymValue f_encryptCbc(ZymVM* vm, ZymValue, ZymValue keyV, ZymValue ivV, ZymValue ptV) {
    PackedByteArray* key; if (!reqBuffer(vm, keyV, "AES.encryptCbc(key, iv, plaintext)", &key)) return ZYM_ERROR;
    if (!checkKeySize(vm, *key, "AES.encryptCbc(key, iv, plaintext)")) return ZYM_ERROR;
    PackedByteArray* iv;  if (!reqBuffer(vm, ivV,  "AES.encryptCbc(key, iv, plaintext)", &iv))  return ZYM_ERROR;
    if (!checkIvSize(vm, *iv, "AES.encryptCbc(key, iv, plaintext)")) return ZYM_ERROR;
    PackedByteArray* pt;  if (!reqBuffer(vm, ptV,  "AES.encryptCbc(key, iv, plaintext)", &pt))  return ZYM_ERROR;

    PackedByteArray padded = pkcs7Pad(*pt);
    Ref<AESContext> c; c.instantiate();
    if (c->start(AESContext::MODE_CBC_ENCRYPT, *key, *iv) != OK) {
        zym_runtimeError(vm, "AES.encryptCbc: failed to initialize");
        return ZYM_ERROR;
    }
    PackedByteArray out = c->update(padded);
    c->finish();
    return makeBufferInstance(vm, out);
}

static ZymValue f_decryptCbc(ZymVM* vm, ZymValue, ZymValue keyV, ZymValue ivV, ZymValue ctV) {
    PackedByteArray* key; if (!reqBuffer(vm, keyV, "AES.decryptCbc(key, iv, ciphertext)", &key)) return ZYM_ERROR;
    if (!checkKeySize(vm, *key, "AES.decryptCbc(key, iv, ciphertext)")) return ZYM_ERROR;
    PackedByteArray* iv;  if (!reqBuffer(vm, ivV,  "AES.decryptCbc(key, iv, ciphertext)", &iv))  return ZYM_ERROR;
    if (!checkIvSize(vm, *iv, "AES.decryptCbc(key, iv, ciphertext)")) return ZYM_ERROR;
    PackedByteArray* ct;  if (!reqBuffer(vm, ctV,  "AES.decryptCbc(key, iv, ciphertext)", &ct))  return ZYM_ERROR;

    if (ct->size() == 0 || (ct->size() % 16) != 0) {
        return zym_newNull();
    }

    Ref<AESContext> c; c.instantiate();
    if (c->start(AESContext::MODE_CBC_DECRYPT, *key, *iv) != OK) {
        zym_runtimeError(vm, "AES.decryptCbc: failed to initialize");
        return ZYM_ERROR;
    }
    PackedByteArray padded = c->update(*ct);
    c->finish();
    PackedByteArray out;
    if (!pkcs7Unpad(padded, &out)) {
        return zym_newNull();
    }
    return makeBufferInstance(vm, out);
}

// ---- factory ----

ZymValue nativeAes_create(ZymVM* vm) {
    ZymValue ctxv = zym_createNativeContext(vm, nullptr, nullptr);
    zym_pushRoot(vm, ctxv);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

#define F(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctxv); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)

    F("create",     "create()",                          f_create);
    F("encryptCbc", "encryptCbc(key, iv, plaintext)",    f_encryptCbc);
    F("decryptCbc", "decryptCbc(key, iv, ciphertext)",   f_decryptCbc);

#undef F

    zym_popRoot(vm);
    zym_popRoot(vm);
    return obj;
}
