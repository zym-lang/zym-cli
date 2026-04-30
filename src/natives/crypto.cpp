// Godot-backed Crypto / CryptoKey / X509Certificate native.
//
// Per-instance shape: each handle is a map-of-closures bound to a context
// whose native data is a `Ref<T>*` (heap-allocated, deleted by finalizer).
// Tag keys disambiguate handle types:
//
//   "__crypto__" -> Ref<Crypto>*           (Crypto instance)
//   "__ck__"     -> Ref<CryptoKey>*        (CryptoKey instance)
//   "__cert__"   -> Ref<X509Certificate>*  (X509Certificate instance)
//
// `Crypto` global exposes `create()` plus the static convenience
// `CryptoKey()` / `X509Certificate()` constructors. Crypto instances expose
// `generateRandomBytes`, `generateRsa`, `generateSelfSignedCertificate`,
// `sign`, `verify`, `encrypt`, `decrypt`, `hmacDigest`, and
// `constantTimeCompare`. CryptoKey instances expose `load`, `save`,
// `saveToString`, `loadFromString`, and `isPublicOnly`. X509Certificate
// instances expose `load`, `save`, `saveToString`, and `loadFromString`.
//
// Byte-array payloads are exchanged through the zym Buffer native (which
// wraps `PackedByteArray`), so callers can chain crypto with file/process
// I/O without copying through hex/base64.
#include "core/crypto/crypto.h"
#include "core/crypto/hashing_context.h"
#include "core/error/error_list.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"

#include "natives.hpp"

// Provided by buffer.cpp; used to return Buffer instances and to accept
// Buffer values from script callers.
extern ZymValue makeBufferInstance(ZymVM* vm, const PackedByteArray& src);

// ---- handles ----

struct CryptoHandle { Ref<Crypto>           c; };
struct CKHandle     { Ref<CryptoKey>        k; };
struct CertHandle   { Ref<X509Certificate>  x; };

static void cryptoFinalizer(ZymVM*, void* data) { delete static_cast<CryptoHandle*>(data); }
static void ckFinalizer    (ZymVM*, void* data) { delete static_cast<CKHandle*>(data); }
static void certFinalizer  (ZymVM*, void* data) { delete static_cast<CertHandle*>(data); }

// ---- helpers ----

static ZymValue strZ(ZymVM* vm, const String& s) {
    CharString u = s.utf8();
    return zym_newStringN(vm, u.get_data(), u.length());
}

static bool reqStr(ZymVM* vm, ZymValue v, const char* where, String* out) {
    if (!zym_isString(v)) { zym_runtimeError(vm, "%s expects a string", where); return false; }
    *out = String::utf8(zym_asCString(v)); return true;
}
static bool reqInt(ZymVM* vm, ZymValue v, const char* where, int64_t* out) {
    if (!zym_isNumber(v)) { zym_runtimeError(vm, "%s expects a number", where); return false; }
    *out = (int64_t)zym_asNumber(v); return true;
}
static bool reqBool(ZymVM* vm, ZymValue v, const char* where, bool* out) {
    if (!zym_isBool(v)) { zym_runtimeError(vm, "%s expects a bool", where); return false; }
    *out = zym_asBool(v); return true;
}

// Optional bool in a fixed-arity tail.
static bool optBool(ZymVM* vm, ZymValue v, const char* where, bool dflt, bool* out) {
    if (zym_isNull(v)) { *out = dflt; return true; }
    return reqBool(vm, v, where, out);
}

// Buffer unwrap mirrors buffer.cpp's reqBuffer.
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

// Map a script-side hash type string ("md5"/"sha1"/"sha256",
// case-insensitive) to HashingContext::HashType. Returns false on bad
// input (with error already raised).
static bool reqHashType(ZymVM* vm, ZymValue v, const char* where, HashingContext::HashType* out) {
    String s; if (!reqStr(vm, v, where, &s)) return false;
    String l = s.to_lower();
    if (l == "md5")    { *out = HashingContext::HASH_MD5;    return true; }
    if (l == "sha1")   { *out = HashingContext::HASH_SHA1;   return true; }
    if (l == "sha256") { *out = HashingContext::HASH_SHA256; return true; }
    zym_runtimeError(vm, "%s: unknown hash type '%s' (expected 'md5', 'sha1', or 'sha256')", where, s.utf8().get_data());
    return false;
}

// `Vector<uint8_t>` and `PackedByteArray` are interchangeable in Godot
// (PackedByteArray *is* Vector<uint8_t>), so these are zero-cost shims --
// they exist purely to make call sites read like the Godot API.
static PackedByteArray vecToPba(const Vector<uint8_t>& v) { return v; }
static Vector<uint8_t> pbaToVec(const PackedByteArray& v) { return v; }

static CryptoHandle* unwrapCrypto(ZymValue ctx) { return static_cast<CryptoHandle*>(zym_getNativeData(ctx)); }
static CKHandle*     unwrapCK    (ZymValue ctx) { return static_cast<CKHandle*>    (zym_getNativeData(ctx)); }
static CertHandle*   unwrapCert  (ZymValue ctx) { return static_cast<CertHandle*>  (zym_getNativeData(ctx)); }

// Forward decls.
static ZymValue makeCryptoInstance(ZymVM* vm, Ref<Crypto> c);
static ZymValue makeCryptoKeyInstance(ZymVM* vm, Ref<CryptoKey> k);
static ZymValue makeX509Instance(ZymVM* vm, Ref<X509Certificate> x);

// Pull a `Ref<CryptoKey>` out of either a CryptoKey wrapper map or
// `null`. Returns false on type error (with error already raised).
static bool reqCryptoKey(ZymVM* vm, ZymValue v, const char* where, Ref<CryptoKey>* out) {
    if (zym_isMap(v)) {
        ZymValue ctx = zym_mapGet(vm, v, "__ck__");
        if (ctx != ZYM_ERROR) {
            CKHandle* h = static_cast<CKHandle*>(zym_getNativeData(ctx));
            if (h) { *out = h->k; return true; }
        }
    }
    zym_runtimeError(vm, "%s expects a CryptoKey", where);
    return false;
}

// Public helpers for cross-TU use (sockets.cpp / TLS): extract Ref<>
// handles from script-side CryptoKey / X509Certificate wrapper maps.
// Return false on shape mismatch *without* raising a runtime error
// (callers decide how to report).
bool zymExtractCryptoKey(ZymVM* vm, ZymValue v, Ref<CryptoKey>* out) {
    if (!zym_isMap(v)) return false;
    ZymValue ctx = zym_mapGet(vm, v, "__ck__");
    if (ctx == ZYM_ERROR) return false;
    CKHandle* h = static_cast<CKHandle*>(zym_getNativeData(ctx));
    if (!h) return false;
    *out = h->k;
    return true;
}

bool zymExtractX509(ZymVM* vm, ZymValue v, Ref<X509Certificate>* out) {
    if (!zym_isMap(v)) return false;
    ZymValue ctx = zym_mapGet(vm, v, "__cert__");
    if (ctx == ZYM_ERROR) return false;
    CertHandle* h = static_cast<CertHandle*>(zym_getNativeData(ctx));
    if (!h) return false;
    *out = h->x;
    return true;
}

// ---- CryptoKey instance methods ----

static ZymValue ck_load(ZymVM* vm, ZymValue ctx, ZymValue pathV, ZymValue pubV) {
    CKHandle* h = unwrapCK(ctx);
    if (!h || h->k.is_null()) { zym_runtimeError(vm, "CryptoKey.load: invalid handle"); return ZYM_ERROR; }
    String path; if (!reqStr(vm, pathV, "CryptoKey.load(path, publicOnly)", &path)) return ZYM_ERROR;
    bool pub;    if (!optBool(vm, pubV, "CryptoKey.load(path, publicOnly)", false, &pub)) return ZYM_ERROR;
    return zym_newBool(h->k->load(path, pub) == OK);
}

static ZymValue ck_save(ZymVM* vm, ZymValue ctx, ZymValue pathV, ZymValue pubV) {
    CKHandle* h = unwrapCK(ctx);
    if (!h || h->k.is_null()) { zym_runtimeError(vm, "CryptoKey.save: invalid handle"); return ZYM_ERROR; }
    String path; if (!reqStr(vm, pathV, "CryptoKey.save(path, publicOnly)", &path)) return ZYM_ERROR;
    bool pub;    if (!optBool(vm, pubV, "CryptoKey.save(path, publicOnly)", false, &pub)) return ZYM_ERROR;
    return zym_newBool(h->k->save(path, pub) == OK);
}

static ZymValue ck_saveToString(ZymVM* vm, ZymValue ctx, ZymValue pubV) {
    CKHandle* h = unwrapCK(ctx);
    if (!h || h->k.is_null()) { zym_runtimeError(vm, "CryptoKey.saveToString: invalid handle"); return ZYM_ERROR; }
    bool pub; if (!optBool(vm, pubV, "CryptoKey.saveToString(publicOnly)", false, &pub)) return ZYM_ERROR;
    return strZ(vm, h->k->save_to_string(pub));
}

static ZymValue ck_loadFromString(ZymVM* vm, ZymValue ctx, ZymValue keyV, ZymValue pubV) {
    CKHandle* h = unwrapCK(ctx);
    if (!h || h->k.is_null()) { zym_runtimeError(vm, "CryptoKey.loadFromString: invalid handle"); return ZYM_ERROR; }
    String key; if (!reqStr(vm, keyV, "CryptoKey.loadFromString(pem, publicOnly)", &key)) return ZYM_ERROR;
    bool pub;   if (!optBool(vm, pubV, "CryptoKey.loadFromString(pem, publicOnly)", false, &pub)) return ZYM_ERROR;
    return zym_newBool(h->k->load_from_string(key, pub) == OK);
}

static ZymValue ck_isPublicOnly(ZymVM* vm, ZymValue ctx) {
    CKHandle* h = unwrapCK(ctx);
    if (!h || h->k.is_null()) { zym_runtimeError(vm, "CryptoKey.isPublicOnly: invalid handle"); return ZYM_ERROR; }
    return zym_newBool(h->k->is_public_only());
}

// ---- X509Certificate instance methods ----

static ZymValue x_load(ZymVM* vm, ZymValue ctx, ZymValue pathV) {
    CertHandle* h = unwrapCert(ctx);
    if (!h || h->x.is_null()) { zym_runtimeError(vm, "X509Certificate.load: invalid handle"); return ZYM_ERROR; }
    String path; if (!reqStr(vm, pathV, "X509Certificate.load(path)", &path)) return ZYM_ERROR;
    return zym_newBool(h->x->load(path) == OK);
}

static ZymValue x_save(ZymVM* vm, ZymValue ctx, ZymValue pathV) {
    CertHandle* h = unwrapCert(ctx);
    if (!h || h->x.is_null()) { zym_runtimeError(vm, "X509Certificate.save: invalid handle"); return ZYM_ERROR; }
    String path; if (!reqStr(vm, pathV, "X509Certificate.save(path)", &path)) return ZYM_ERROR;
    return zym_newBool(h->x->save(path) == OK);
}

static ZymValue x_saveToString(ZymVM* vm, ZymValue ctx) {
    CertHandle* h = unwrapCert(ctx);
    if (!h || h->x.is_null()) { zym_runtimeError(vm, "X509Certificate.saveToString: invalid handle"); return ZYM_ERROR; }
    return strZ(vm, h->x->save_to_string());
}

static ZymValue x_loadFromString(ZymVM* vm, ZymValue ctx, ZymValue pemV) {
    CertHandle* h = unwrapCert(ctx);
    if (!h || h->x.is_null()) { zym_runtimeError(vm, "X509Certificate.loadFromString: invalid handle"); return ZYM_ERROR; }
    String pem; if (!reqStr(vm, pemV, "X509Certificate.loadFromString(pem)", &pem)) return ZYM_ERROR;
    return zym_newBool(h->x->load_from_string(pem) == OK);
}

// ---- Crypto instance methods ----

static ZymValue c_generateRandomBytes(ZymVM* vm, ZymValue ctx, ZymValue nV) {
    CryptoHandle* h = unwrapCrypto(ctx);
    if (!h || h->c.is_null()) { zym_runtimeError(vm, "Crypto.generateRandomBytes: invalid handle"); return ZYM_ERROR; }
    int64_t n; if (!reqInt(vm, nV, "Crypto.generateRandomBytes(n)", &n)) return ZYM_ERROR;
    if (n < 0) { zym_runtimeError(vm, "Crypto.generateRandomBytes(n): n must be >= 0"); return ZYM_ERROR; }
    return makeBufferInstance(vm, h->c->generate_random_bytes((int)n));
}

static ZymValue c_generateRsa(ZymVM* vm, ZymValue ctx, ZymValue bitsV) {
    CryptoHandle* h = unwrapCrypto(ctx);
    if (!h || h->c.is_null()) { zym_runtimeError(vm, "Crypto.generateRsa: invalid handle"); return ZYM_ERROR; }
    int64_t bits; if (!reqInt(vm, bitsV, "Crypto.generateRsa(bits)", &bits)) return ZYM_ERROR;
    Ref<CryptoKey> k = h->c->generate_rsa((int)bits);
    if (k.is_null()) return zym_newNull();
    return makeCryptoKeyInstance(vm, k);
}

static ZymValue c_generateSelfSignedCertificate(ZymVM* vm, ZymValue ctx, ZymValue keyV, ZymValue issuerV, ZymValue notBeforeV, ZymValue notAfterV) {
    CryptoHandle* h = unwrapCrypto(ctx);
    if (!h || h->c.is_null()) { zym_runtimeError(vm, "Crypto.generateSelfSignedCertificate: invalid handle"); return ZYM_ERROR; }
    Ref<CryptoKey> key; if (!reqCryptoKey(vm, keyV, "Crypto.generateSelfSignedCertificate(key, issuer, notBefore, notAfter)", &key)) return ZYM_ERROR;
    String issuer, notBefore, notAfter;
    if (!reqStr(vm, issuerV,    "Crypto.generateSelfSignedCertificate(...) issuer",    &issuer))    return ZYM_ERROR;
    if (!reqStr(vm, notBeforeV, "Crypto.generateSelfSignedCertificate(...) notBefore", &notBefore)) return ZYM_ERROR;
    if (!reqStr(vm, notAfterV,  "Crypto.generateSelfSignedCertificate(...) notAfter",  &notAfter))  return ZYM_ERROR;
    Ref<X509Certificate> x = h->c->generate_self_signed_certificate(key, issuer, notBefore, notAfter);
    if (x.is_null()) return zym_newNull();
    return makeX509Instance(vm, x);
}

static ZymValue c_sign(ZymVM* vm, ZymValue ctx, ZymValue hashTypeV, ZymValue hashV, ZymValue keyV) {
    CryptoHandle* h = unwrapCrypto(ctx);
    if (!h || h->c.is_null()) { zym_runtimeError(vm, "Crypto.sign: invalid handle"); return ZYM_ERROR; }
    HashingContext::HashType ht; if (!reqHashType(vm, hashTypeV, "Crypto.sign(hashType, hash, key)", &ht)) return ZYM_ERROR;
    PackedByteArray* hash; if (!reqBuffer(vm, hashV, "Crypto.sign(hashType, hash, key)", &hash)) return ZYM_ERROR;
    Ref<CryptoKey> key; if (!reqCryptoKey(vm, keyV, "Crypto.sign(hashType, hash, key)", &key)) return ZYM_ERROR;
    Vector<uint8_t> sig = h->c->sign(ht, pbaToVec(*hash), key);
    if (sig.is_empty()) return zym_newNull();
    return makeBufferInstance(vm, vecToPba(sig));
}

static ZymValue c_verify(ZymVM* vm, ZymValue ctx, ZymValue hashTypeV, ZymValue hashV, ZymValue sigV, ZymValue keyV) {
    CryptoHandle* h = unwrapCrypto(ctx);
    if (!h || h->c.is_null()) { zym_runtimeError(vm, "Crypto.verify: invalid handle"); return ZYM_ERROR; }
    HashingContext::HashType ht; if (!reqHashType(vm, hashTypeV, "Crypto.verify(hashType, hash, signature, key)", &ht)) return ZYM_ERROR;
    PackedByteArray* hash; if (!reqBuffer(vm, hashV, "Crypto.verify(hashType, hash, signature, key)", &hash)) return ZYM_ERROR;
    PackedByteArray* sig;  if (!reqBuffer(vm, sigV,  "Crypto.verify(hashType, hash, signature, key)", &sig))  return ZYM_ERROR;
    Ref<CryptoKey> key; if (!reqCryptoKey(vm, keyV, "Crypto.verify(hashType, hash, signature, key)", &key)) return ZYM_ERROR;
    return zym_newBool(h->c->verify(ht, pbaToVec(*hash), pbaToVec(*sig), key));
}

static ZymValue c_encrypt(ZymVM* vm, ZymValue ctx, ZymValue keyV, ZymValue ptV) {
    CryptoHandle* h = unwrapCrypto(ctx);
    if (!h || h->c.is_null()) { zym_runtimeError(vm, "Crypto.encrypt: invalid handle"); return ZYM_ERROR; }
    Ref<CryptoKey> key; if (!reqCryptoKey(vm, keyV, "Crypto.encrypt(key, plaintext)", &key)) return ZYM_ERROR;
    PackedByteArray* pt; if (!reqBuffer(vm, ptV, "Crypto.encrypt(key, plaintext)", &pt)) return ZYM_ERROR;
    Vector<uint8_t> ct = h->c->encrypt(key, pbaToVec(*pt));
    if (ct.is_empty()) return zym_newNull();
    return makeBufferInstance(vm, vecToPba(ct));
}

static ZymValue c_decrypt(ZymVM* vm, ZymValue ctx, ZymValue keyV, ZymValue ctV) {
    CryptoHandle* h = unwrapCrypto(ctx);
    if (!h || h->c.is_null()) { zym_runtimeError(vm, "Crypto.decrypt: invalid handle"); return ZYM_ERROR; }
    Ref<CryptoKey> key; if (!reqCryptoKey(vm, keyV, "Crypto.decrypt(key, ciphertext)", &key)) return ZYM_ERROR;
    PackedByteArray* ct; if (!reqBuffer(vm, ctV, "Crypto.decrypt(key, ciphertext)", &ct)) return ZYM_ERROR;
    Vector<uint8_t> pt = h->c->decrypt(key, pbaToVec(*ct));
    if (pt.is_empty()) return zym_newNull();
    return makeBufferInstance(vm, vecToPba(pt));
}

static ZymValue c_hmacDigest(ZymVM* vm, ZymValue ctx, ZymValue hashTypeV, ZymValue keyV, ZymValue msgV) {
    CryptoHandle* h = unwrapCrypto(ctx);
    if (!h || h->c.is_null()) { zym_runtimeError(vm, "Crypto.hmacDigest: invalid handle"); return ZYM_ERROR; }
    HashingContext::HashType ht; if (!reqHashType(vm, hashTypeV, "Crypto.hmacDigest(hashType, key, msg)", &ht)) return ZYM_ERROR;
    PackedByteArray* key; if (!reqBuffer(vm, keyV, "Crypto.hmacDigest(hashType, key, msg)", &key)) return ZYM_ERROR;
    PackedByteArray* msg; if (!reqBuffer(vm, msgV, "Crypto.hmacDigest(hashType, key, msg)", &msg)) return ZYM_ERROR;
    return makeBufferInstance(vm, h->c->hmac_digest(ht, *key, *msg));
}

static ZymValue c_constantTimeCompare(ZymVM* vm, ZymValue ctx, ZymValue aV, ZymValue bV) {
    CryptoHandle* h = unwrapCrypto(ctx);
    if (!h || h->c.is_null()) { zym_runtimeError(vm, "Crypto.constantTimeCompare: invalid handle"); return ZYM_ERROR; }
    PackedByteArray* a; if (!reqBuffer(vm, aV, "Crypto.constantTimeCompare(trusted, received)", &a)) return ZYM_ERROR;
    PackedByteArray* b; if (!reqBuffer(vm, bV, "Crypto.constantTimeCompare(trusted, received)", &b)) return ZYM_ERROR;
    return zym_newBool(h->c->constant_time_compare(*a, *b));
}

// ---- instance assembly ----

static ZymValue makeCryptoKeyInstance(ZymVM* vm, Ref<CryptoKey> k) {
    auto* data = new CKHandle{ k };
    ZymValue ctxv = zym_createNativeContext(vm, data, ckFinalizer);
    zym_pushRoot(vm, ctxv);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__ck__", ctxv);

#define M(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctxv); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)

    M("load",           "load(path, publicOnly)",         ck_load);
    M("save",           "save(path, publicOnly)",         ck_save);
    M("saveToString",   "saveToString(publicOnly)",       ck_saveToString);
    M("loadFromString", "loadFromString(pem, publicOnly)", ck_loadFromString);
    M("isPublicOnly",   "isPublicOnly()",                 ck_isPublicOnly);

#undef M

    zym_popRoot(vm);
    zym_popRoot(vm);
    return obj;
}

static ZymValue makeX509Instance(ZymVM* vm, Ref<X509Certificate> x) {
    auto* data = new CertHandle{ x };
    ZymValue ctxv = zym_createNativeContext(vm, data, certFinalizer);
    zym_pushRoot(vm, ctxv);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__cert__", ctxv);

#define M(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctxv); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)

    M("load",           "load(path)",            x_load);
    M("save",           "save(path)",            x_save);
    M("saveToString",   "saveToString()",        x_saveToString);
    M("loadFromString", "loadFromString(pem)",   x_loadFromString);

#undef M

    zym_popRoot(vm);
    zym_popRoot(vm);
    return obj;
}

static ZymValue makeCryptoInstance(ZymVM* vm, Ref<Crypto> c) {
    auto* data = new CryptoHandle{ c };
    ZymValue ctxv = zym_createNativeContext(vm, data, cryptoFinalizer);
    zym_pushRoot(vm, ctxv);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__crypto__", ctxv);

#define M(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctxv); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)

    M("generateRandomBytes",            "generateRandomBytes(n)",                                       c_generateRandomBytes);
    M("generateRsa",                    "generateRsa(bits)",                                            c_generateRsa);
    M("generateSelfSignedCertificate",  "generateSelfSignedCertificate(key, issuer, notBefore, notAfter)", c_generateSelfSignedCertificate);
    M("sign",                           "sign(hashType, hash, key)",                                    c_sign);
    M("verify",                         "verify(hashType, hash, signature, key)",                       c_verify);
    M("encrypt",                        "encrypt(key, plaintext)",                                      c_encrypt);
    M("decrypt",                        "decrypt(key, ciphertext)",                                     c_decrypt);
    M("hmacDigest",                     "hmacDigest(hashType, key, msg)",                               c_hmacDigest);
    M("constantTimeCompare",            "constantTimeCompare(trusted, received)",                       c_constantTimeCompare);

#undef M

    zym_popRoot(vm);
    zym_popRoot(vm);
    return obj;
}

// ---- Crypto global (statics) ----

static ZymValue f_create(ZymVM* vm, ZymValue) {
    Ref<Crypto> c = Ref<Crypto>(Crypto::create());
    if (c.is_null()) return zym_newNull();
    return makeCryptoInstance(vm, c);
}

static ZymValue f_cryptoKey(ZymVM* vm, ZymValue) {
    Ref<CryptoKey> k = Ref<CryptoKey>(CryptoKey::create());
    if (k.is_null()) return zym_newNull();
    return makeCryptoKeyInstance(vm, k);
}

static ZymValue f_x509Certificate(ZymVM* vm, ZymValue) {
    Ref<X509Certificate> x = Ref<X509Certificate>(X509Certificate::create());
    if (x.is_null()) return zym_newNull();
    return makeX509Instance(vm, x);
}

// ---- factory ----

ZymValue nativeCrypto_create(ZymVM* vm) {
    ZymValue ctxv = zym_createNativeContext(vm, nullptr, nullptr);
    zym_pushRoot(vm, ctxv);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

#define F(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctxv); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)

    F("create",           "create()",           f_create);
    F("CryptoKey",        "CryptoKey()",        f_cryptoKey);
    F("X509Certificate",  "X509Certificate()",  f_x509Certificate);

#undef F

    zym_popRoot(vm);
    zym_popRoot(vm);
    return obj;
}
