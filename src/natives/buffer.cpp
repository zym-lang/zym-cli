// Godot-backed Buffer (PackedByteArray wrapper).
// Per-instance: each buffer is a map-of-closures bound to a context whose
// native data is a `new PackedByteArray` (deleted by the finalizer).
// CoW value-semantics: mutation methods detach via ptrw() when shared.
#include "core/io/compression.h"
#include "core/io/marshalls.h"
#include "core/math/math_funcs.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/variant.h"

#include <zlib.h>

#include "natives.hpp"

// ---- helpers ----

static void bufFinalizer(ZymVM*, void* data) {
    delete static_cast<PackedByteArray*>(data);
}

static PackedByteArray* unwrap(ZymValue ctx) {
    return static_cast<PackedByteArray*>(zym_getNativeData(ctx));
}

static ZymValue stringToZym(ZymVM* vm, const String& s) {
    CharString u = s.utf8();
    return zym_newStringN(vm, u.get_data(), u.length());
}

static bool reqNum(ZymVM* vm, ZymValue v, const char* where, double* out) {
    if (!zym_isNumber(v)) { zym_runtimeError(vm, "%s expects a number", where); return false; }
    *out = zym_asNumber(v); return true;
}

static bool reqInt(ZymVM* vm, ZymValue v, const char* where, int64_t* out) {
    double d;
    if (!reqNum(vm, v, where, &d)) return false;
    *out = (int64_t)d; return true;
}

static bool reqBool(ZymVM* vm, ZymValue v, const char* where, bool* out) {
    if (!zym_isBool(v)) { zym_runtimeError(vm, "%s expects a bool", where); return false; }
    *out = zym_asBool(v); return true;
}

static bool reqString(ZymVM* vm, ZymValue v, const char* where, String* out) {
    if (!zym_isString(v)) { zym_runtimeError(vm, "%s expects a string", where); return false; }
    *out = String::utf8(zym_asCString(v)); return true;
}

// Forward decl: builds an instance map wrapping a fresh PBA copy.
static ZymValue makeInstance(ZymVM* vm, const PackedByteArray& src);

// External entry point for other natives (e.g. File) that need to return buffers.
ZymValue makeBufferInstance(ZymVM* vm, const PackedByteArray& src) {
    return makeInstance(vm, src);
}

// Type-clean wrappers used by the `Zym` native (which lives outside the
// Godot include scope and so cannot mention `PackedByteArray`).
ZymValue makeBufferFromBytes(ZymVM* vm, const char* data, size_t size) {
    PackedByteArray pba;
    if (size > 0) {
        pba.resize((int)size);
        if (data) memcpy(pba.ptrw(), data, size);
    }
    return makeInstance(vm, pba);
}

bool readBufferBytes(ZymVM* vm, ZymValue v, const char** out_data, size_t* out_size) {
    if (!zym_isMap(v)) return false;
    ZymValue ctx = zym_mapGet(vm, v, "__pba__");
    if (ctx == ZYM_ERROR) return false;
    void* data = zym_getNativeData(ctx);
    if (!data) return false;
    auto* pba = static_cast<PackedByteArray*>(data);
    if (out_data) *out_data = (const char*)pba->ptr();
    if (out_size) *out_size = (size_t)pba->size();
    return true;
}

// Mirror of `readBufferBytes` for the write direction. Used by
// `Pack.editBuffer`'s commit path: rewrites the borrowed
// PackedByteArray in place so every live script reference to the
// same Buffer observes the new contents (out-param / mutator style).
bool writeBufferBytes(ZymVM* vm, ZymValue v, const void* data, size_t size) {
    if (!zym_isMap(v)) return false;
    ZymValue ctx = zym_mapGet(vm, v, "__pba__");
    if (ctx == ZYM_ERROR) return false;
    void* nd = zym_getNativeData(ctx);
    if (!nd) return false;
    auto* pba = static_cast<PackedByteArray*>(nd);
    if (pba->resize((int)size) != 0) return false;
    if (size > 0 && data) memcpy(pba->ptrw(), data, size);
    return true;
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

// Bulk/mask helpers: every elementwise op requires operand buffers to match
// the receiver's length exactly. Silent zero-pad or truncate would mask bugs.
static bool reqSameSize(ZymVM* vm, PackedByteArray* dst, PackedByteArray* op,
                        const char* where, const char* opname) {
    if (dst->size() != op->size()) {
        zym_runtimeError(vm, "%s: %s size %d does not match receiver size %d",
                         where, opname, (int)op->size(), (int)dst->size());
        return false;
    }
    return true;
}

// Saturating u8 arithmetic: clamp to [0, 255] after a wider add/sub so that
// chained ops match the "byte values masked to 8 bits" doc contract without
// silently wrapping past the edges.
static inline uint8_t sat_add_u8(int a, int b) {
    int r = a + b;
    return (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
}
static inline uint8_t sat_sub_u8(int a, int b) {
    int r = a - b;
    return (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
}

// ---- instance methods ----

static ZymValue b_size(ZymVM*, ZymValue ctx) {
    return zym_newNumber((double)unwrap(ctx)->size());
}
static ZymValue b_isEmpty(ZymVM*, ZymValue ctx) {
    return zym_newBool(unwrap(ctx)->is_empty());
}
static ZymValue b_clear(ZymVM*, ZymValue ctx) {
    unwrap(ctx)->clear(); return zym_newNull();
}
static ZymValue b_resize(ZymVM* vm, ZymValue ctx, ZymValue nv) {
    int64_t n; if (!reqInt(vm, nv, "Buffer.resize(n)", &n)) return ZYM_ERROR;
    if (n < 0) { zym_runtimeError(vm, "Buffer.resize(n): n must be >= 0"); return ZYM_ERROR; }
    return zym_newNumber((double)unwrap(ctx)->resize(n));
}
static ZymValue b_fill(ZymVM* vm, ZymValue ctx, ZymValue vv) {
    int64_t v; if (!reqInt(vm, vv, "Buffer.fill(v)", &v)) return ZYM_ERROR;
    unwrap(ctx)->fill((uint8_t)v); return zym_newNull();
}
static ZymValue b_duplicate(ZymVM* vm, ZymValue ctx) {
    return makeInstance(vm, unwrap(ctx)->duplicate());
}
static ZymValue b_get(ZymVM* vm, ZymValue ctx, ZymValue iv) {
    int64_t i; if (!reqInt(vm, iv, "Buffer.get(i)", &i)) return ZYM_ERROR;
    auto* p = unwrap(ctx);
    if (i < 0 || i >= p->size()) { zym_runtimeError(vm, "Buffer.get(i): index out of range"); return ZYM_ERROR; }
    return zym_newNumber((double)p->get(i));
}
static ZymValue b_set(ZymVM* vm, ZymValue ctx, ZymValue iv, ZymValue vv) {
    int64_t i, v;
    if (!reqInt(vm, iv, "Buffer.set(i, v)", &i)) return ZYM_ERROR;
    if (!reqInt(vm, vv, "Buffer.set(i, v)", &v)) return ZYM_ERROR;
    auto* p = unwrap(ctx);
    if (i < 0 || i >= p->size()) { zym_runtimeError(vm, "Buffer.set(i, v): index out of range"); return ZYM_ERROR; }
    p->set(i, (uint8_t)v);
    return zym_newNull();
}
static ZymValue b_append(ZymVM* vm, ZymValue ctx, ZymValue vv) {
    int64_t v; if (!reqInt(vm, vv, "Buffer.append(v)", &v)) return ZYM_ERROR;
    return zym_newBool(unwrap(ctx)->append((uint8_t)v));
}
static ZymValue b_insert(ZymVM* vm, ZymValue ctx, ZymValue iv, ZymValue vv) {
    int64_t i, v;
    if (!reqInt(vm, iv, "Buffer.insert(i, v)", &i)) return ZYM_ERROR;
    if (!reqInt(vm, vv, "Buffer.insert(i, v)", &v)) return ZYM_ERROR;
    return zym_newNumber((double)unwrap(ctx)->insert(i, (uint8_t)v));
}
static ZymValue b_removeAt(ZymVM* vm, ZymValue ctx, ZymValue iv) {
    int64_t i; if (!reqInt(vm, iv, "Buffer.removeAt(i)", &i)) return ZYM_ERROR;
    unwrap(ctx)->remove_at(i); return zym_newNull();
}
static ZymValue b_erase(ZymVM* vm, ZymValue ctx, ZymValue vv) {
    int64_t v; if (!reqInt(vm, vv, "Buffer.erase(v)", &v)) return ZYM_ERROR;
    return zym_newBool(unwrap(ctx)->erase((uint8_t)v));
}
static ZymValue b_reverse(ZymVM*, ZymValue ctx) {
    unwrap(ctx)->reverse(); return zym_newNull();
}
static ZymValue b_sort(ZymVM*, ZymValue ctx) {
    unwrap(ctx)->sort(); return zym_newNull();
}
static ZymValue b_has(ZymVM* vm, ZymValue ctx, ZymValue vv) {
    int64_t v; if (!reqInt(vm, vv, "Buffer.has(v)", &v)) return ZYM_ERROR;
    return zym_newBool(unwrap(ctx)->has((uint8_t)v));
}
static ZymValue b_find(ZymVM* vm, ZymValue ctx, ZymValue vv, ZymValue fromV) {
    int64_t v, from;
    if (!reqInt(vm, vv,    "Buffer.find(v, from)", &v))    return ZYM_ERROR;
    if (!reqInt(vm, fromV, "Buffer.find(v, from)", &from)) return ZYM_ERROR;
    return zym_newNumber((double)unwrap(ctx)->find((uint8_t)v, (int)from));
}
static ZymValue b_rfind(ZymVM* vm, ZymValue ctx, ZymValue vv, ZymValue fromV) {
    int64_t v, from;
    if (!reqInt(vm, vv,    "Buffer.rfind(v, from)", &v))    return ZYM_ERROR;
    if (!reqInt(vm, fromV, "Buffer.rfind(v, from)", &from)) return ZYM_ERROR;
    return zym_newNumber((double)unwrap(ctx)->rfind((uint8_t)v, (int)from));
}
static ZymValue b_count(ZymVM* vm, ZymValue ctx, ZymValue vv) {
    int64_t v; if (!reqInt(vm, vv, "Buffer.count(v)", &v)) return ZYM_ERROR;
    return zym_newNumber((double)unwrap(ctx)->count((uint8_t)v));
}
static ZymValue b_bsearch(ZymVM* vm, ZymValue ctx, ZymValue vv, ZymValue beforeV) {
    int64_t v; bool before;
    if (!reqInt(vm, vv,       "Buffer.bsearch(v, before)", &v))      return ZYM_ERROR;
    if (!reqBool(vm, beforeV, "Buffer.bsearch(v, before)", &before)) return ZYM_ERROR;
    return zym_newNumber((double)unwrap(ctx)->bsearch((uint8_t)v, before));
}
static ZymValue b_slice(ZymVM* vm, ZymValue ctx, ZymValue beginV, ZymValue endV) {
    int64_t begin, end;
    if (!reqInt(vm, beginV, "Buffer.slice(begin, end)", &begin)) return ZYM_ERROR;
    if (!reqInt(vm, endV,   "Buffer.slice(begin, end)", &end))   return ZYM_ERROR;
    return makeInstance(vm, unwrap(ctx)->slice((int)begin, (int)end));
}
static ZymValue b_equals(ZymVM* vm, ZymValue ctx, ZymValue other) {
    PackedByteArray* o; if (!reqBuffer(vm, other, "Buffer.equals(other)", &o)) return ZYM_ERROR;
    return zym_newBool(*unwrap(ctx) == *o);
}
static ZymValue b_concat(ZymVM* vm, ZymValue ctx, ZymValue other) {
    PackedByteArray* o; if (!reqBuffer(vm, other, "Buffer.concat(other)", &o)) return ZYM_ERROR;
    PackedByteArray out = *unwrap(ctx);
    out.append_array(*o);
    return makeInstance(vm, out);
}
static ZymValue b_hex(ZymVM* vm, ZymValue ctx) {
    auto* p = unwrap(ctx);
    if (p->size() == 0) return stringToZym(vm, String());
    return stringToZym(vm, String::hex_encode_buffer(p->ptr(), p->size()));
}
static ZymValue b_toUtf8(ZymVM* vm, ZymValue ctx) {
    auto* p = unwrap(ctx);
    String s;
    if (p->size() > 0) {
        // Treat the buffer as a NUL-terminated C string: stop at the
        // first zero byte so callers using a Buffer as a text holder
        // (e.g. ImGui's inputText, which pads with NULs up to capacity)
        // get just the actual string content, not the padding.
        size_t n = strnlen((const char*)p->ptr(), (size_t)p->size());
        if (n > 0) s.append_utf8((const char*)p->ptr(), (int)n);
    }
    return stringToZym(vm, s);
}
static ZymValue b_toAscii(ZymVM* vm, ZymValue ctx) {
    auto* p = unwrap(ctx);
    String s;
    if (p->size() > 0) {
        size_t n = strnlen((const char*)p->ptr(), (size_t)p->size());
        if (n > 0) s.append_ascii(Span<char>((const char*)p->ptr(), (int)n));
    }
    return stringToZym(vm, s);
}

// ---- bulk / mask operations ----
//
// Convention: a "mask" is a Buffer where byte == 0 means OFF and any non-zero
// byte means ON. Predicates produce strict 0/1 so they compose cleanly with
// the bitwise ops; the read convention stays permissive so masks built from
// other sources (network frames, parser output) still work without a manual
// normalisation pass.
//
// Arithmetic ops saturate at [0, 255] — wrap is recoverable via the bitwise
// scalar ops. Comparisons are unsigned (bytes are u8). All elementwise
// methods require operand buffers to match the receiver's size exactly.
// Receiver may alias an operand for every per-byte op (each write is local
// to its index); the one exception is mapU8's LUT, which is checked.

// ---- predicates (mask construction) ----

#define BUF_PREDICATE_SCALAR(name, op) \
    static ZymValue b_##name(ZymVM* vm, ZymValue ctx, ZymValue srcV, ZymValue vV) { \
        PackedByteArray* src; if (!reqBuffer(vm, srcV, "Buffer." #name "(src, v)", &src)) return ZYM_ERROR; \
        int64_t v; if (!reqInt(vm, vV, "Buffer." #name "(src, v)", &v)) return ZYM_ERROR; \
        auto* dst = unwrap(ctx); \
        if (!reqSameSize(vm, dst, src, "Buffer." #name "(src, v)", "src")) return ZYM_ERROR; \
        uint8_t t = (uint8_t)v; \
        int64_t n = dst->size(); \
        const uint8_t* s = src->ptr(); \
        uint8_t* d = dst->ptrw(); \
        for (int64_t i = 0; i < n; ++i) d[i] = (s[i] op t) ? 1 : 0; \
        return zym_newNull(); \
    }

BUF_PREDICATE_SCALAR(eqScalar,  ==)
BUF_PREDICATE_SCALAR(neqScalar, !=)
BUF_PREDICATE_SCALAR(ltScalar,  <)
BUF_PREDICATE_SCALAR(leScalar,  <=)
BUF_PREDICATE_SCALAR(gtScalar,  >)
BUF_PREDICATE_SCALAR(geScalar,  >=)

#undef BUF_PREDICATE_SCALAR

static ZymValue b_inRange(ZymVM* vm, ZymValue ctx, ZymValue srcV, ZymValue loV, ZymValue hiV) {
    PackedByteArray* src; if (!reqBuffer(vm, srcV, "Buffer.inRange(src, lo, hi)", &src)) return ZYM_ERROR;
    int64_t lo, hi;
    if (!reqInt(vm, loV, "Buffer.inRange(src, lo, hi)", &lo)) return ZYM_ERROR;
    if (!reqInt(vm, hiV, "Buffer.inRange(src, lo, hi)", &hi)) return ZYM_ERROR;
    auto* dst = unwrap(ctx);
    if (!reqSameSize(vm, dst, src, "Buffer.inRange(src, lo, hi)", "src")) return ZYM_ERROR;
    uint8_t l = (uint8_t)lo, h = (uint8_t)hi;
    int64_t n = dst->size();
    const uint8_t* s = src->ptr();
    uint8_t* d = dst->ptrw();
    for (int64_t i = 0; i < n; ++i) d[i] = (s[i] >= l && s[i] <= h) ? 1 : 0;
    return zym_newNull();
}

#define BUF_PREDICATE_BUFFER(name, op) \
    static ZymValue b_##name(ZymVM* vm, ZymValue ctx, ZymValue aV, ZymValue bV) { \
        PackedByteArray* sa; if (!reqBuffer(vm, aV, "Buffer." #name "(srcA, srcB)", &sa)) return ZYM_ERROR; \
        PackedByteArray* sb; if (!reqBuffer(vm, bV, "Buffer." #name "(srcA, srcB)", &sb)) return ZYM_ERROR; \
        auto* dst = unwrap(ctx); \
        if (!reqSameSize(vm, dst, sa, "Buffer." #name "(srcA, srcB)", "srcA")) return ZYM_ERROR; \
        if (!reqSameSize(vm, dst, sb, "Buffer." #name "(srcA, srcB)", "srcB")) return ZYM_ERROR; \
        int64_t n = dst->size(); \
        const uint8_t* a = sa->ptr(); \
        const uint8_t* b = sb->ptr(); \
        uint8_t* d = dst->ptrw(); \
        for (int64_t i = 0; i < n; ++i) d[i] = (a[i] op b[i]) ? 1 : 0; \
        return zym_newNull(); \
    }

BUF_PREDICATE_BUFFER(eqBuffer,  ==)
BUF_PREDICATE_BUFFER(neqBuffer, !=)
BUF_PREDICATE_BUFFER(ltBuffer,  <)
BUF_PREDICATE_BUFFER(leBuffer,  <=)
BUF_PREDICATE_BUFFER(gtBuffer,  >)
BUF_PREDICATE_BUFFER(geBuffer,  >=)

#undef BUF_PREDICATE_BUFFER

// ---- bitwise (per-byte; also serve as mask combinators on strict 0/1) ----

#define BUF_BITWISE_BUFFER(name, op) \
    static ZymValue b_##name(ZymVM* vm, ZymValue ctx, ZymValue otherV) { \
        PackedByteArray* o; if (!reqBuffer(vm, otherV, "Buffer." #name "(other)", &o)) return ZYM_ERROR; \
        auto* dst = unwrap(ctx); \
        if (!reqSameSize(vm, dst, o, "Buffer." #name "(other)", "other")) return ZYM_ERROR; \
        int64_t n = dst->size(); \
        const uint8_t* s = o->ptr(); \
        uint8_t* d = dst->ptrw(); \
        for (int64_t i = 0; i < n; ++i) d[i] = (uint8_t)(d[i] op s[i]); \
        return zym_newNull(); \
    }

BUF_BITWISE_BUFFER(bitAnd, &)
BUF_BITWISE_BUFFER(bitOr,  |)
BUF_BITWISE_BUFFER(bitXor, ^)

#undef BUF_BITWISE_BUFFER

static ZymValue b_bitNot(ZymVM*, ZymValue ctx) {
    auto* dst = unwrap(ctx);
    int64_t n = dst->size();
    uint8_t* d = dst->ptrw();
    for (int64_t i = 0; i < n; ++i) d[i] = (uint8_t)(~d[i]);
    return zym_newNull();
}

#define BUF_BITWISE_SCALAR(name, op) \
    static ZymValue b_##name(ZymVM* vm, ZymValue ctx, ZymValue vV) { \
        int64_t v; if (!reqInt(vm, vV, "Buffer." #name "(v)", &v)) return ZYM_ERROR; \
        auto* dst = unwrap(ctx); \
        uint8_t t = (uint8_t)v; \
        int64_t n = dst->size(); \
        uint8_t* d = dst->ptrw(); \
        for (int64_t i = 0; i < n; ++i) d[i] = (uint8_t)(d[i] op t); \
        return zym_newNull(); \
    }

BUF_BITWISE_SCALAR(bitAndScalar, &)
BUF_BITWISE_SCALAR(bitOrScalar,  |)
BUF_BITWISE_SCALAR(bitXorScalar, ^)

#undef BUF_BITWISE_SCALAR

// ---- reductions ----

static ZymValue b_countNonZero(ZymVM*, ZymValue ctx) {
    auto* p = unwrap(ctx);
    int64_t n = p->size();
    const uint8_t* s = p->ptr();
    int64_t c = 0;
    for (int64_t i = 0; i < n; ++i) if (s[i] != 0) c++;
    return zym_newNumber((double)c);
}

static ZymValue b_any(ZymVM*, ZymValue ctx) {
    auto* p = unwrap(ctx);
    int64_t n = p->size();
    const uint8_t* s = p->ptr();
    for (int64_t i = 0; i < n; ++i) if (s[i] != 0) return zym_newBool(true);
    return zym_newBool(false);
}

static ZymValue b_all(ZymVM*, ZymValue ctx) {
    auto* p = unwrap(ctx);
    int64_t n = p->size();
    if (n == 0) return zym_newBool(true);
    const uint8_t* s = p->ptr();
    for (int64_t i = 0; i < n; ++i) if (s[i] == 0) return zym_newBool(false);
    return zym_newBool(true);
}

// ---- masked writes ----

static ZymValue b_maskedFill(ZymVM* vm, ZymValue ctx, ZymValue maskV, ZymValue vV) {
    PackedByteArray* m; if (!reqBuffer(vm, maskV, "Buffer.maskedFill(mask, v)", &m)) return ZYM_ERROR;
    int64_t v; if (!reqInt(vm, vV, "Buffer.maskedFill(mask, v)", &v)) return ZYM_ERROR;
    auto* dst = unwrap(ctx);
    if (!reqSameSize(vm, dst, m, "Buffer.maskedFill(mask, v)", "mask")) return ZYM_ERROR;
    uint8_t t = (uint8_t)v;
    int64_t n = dst->size();
    const uint8_t* mp = m->ptr();
    uint8_t* d = dst->ptrw();
    for (int64_t i = 0; i < n; ++i) if (mp[i]) d[i] = t;
    return zym_newNull();
}

static ZymValue b_maskedCopy(ZymVM* vm, ZymValue ctx, ZymValue maskV, ZymValue srcV) {
    PackedByteArray* m; if (!reqBuffer(vm, maskV, "Buffer.maskedCopy(mask, src)", &m)) return ZYM_ERROR;
    PackedByteArray* s; if (!reqBuffer(vm, srcV,  "Buffer.maskedCopy(mask, src)", &s)) return ZYM_ERROR;
    auto* dst = unwrap(ctx);
    if (!reqSameSize(vm, dst, m, "Buffer.maskedCopy(mask, src)", "mask")) return ZYM_ERROR;
    if (!reqSameSize(vm, dst, s, "Buffer.maskedCopy(mask, src)", "src"))  return ZYM_ERROR;
    int64_t n = dst->size();
    const uint8_t* mp = m->ptr();
    const uint8_t* sp = s->ptr();
    uint8_t* d = dst->ptrw();
    for (int64_t i = 0; i < n; ++i) if (mp[i]) d[i] = sp[i];
    return zym_newNull();
}

static ZymValue b_select(ZymVM* vm, ZymValue ctx, ZymValue maskV, ZymValue aV, ZymValue bV) {
    PackedByteArray* m;  if (!reqBuffer(vm, maskV, "Buffer.select(mask, srcA, srcB)", &m))  return ZYM_ERROR;
    PackedByteArray* sa; if (!reqBuffer(vm, aV,    "Buffer.select(mask, srcA, srcB)", &sa)) return ZYM_ERROR;
    PackedByteArray* sb; if (!reqBuffer(vm, bV,    "Buffer.select(mask, srcA, srcB)", &sb)) return ZYM_ERROR;
    auto* dst = unwrap(ctx);
    if (!reqSameSize(vm, dst, m,  "Buffer.select(mask, srcA, srcB)", "mask")) return ZYM_ERROR;
    if (!reqSameSize(vm, dst, sa, "Buffer.select(mask, srcA, srcB)", "srcA")) return ZYM_ERROR;
    if (!reqSameSize(vm, dst, sb, "Buffer.select(mask, srcA, srcB)", "srcB")) return ZYM_ERROR;
    int64_t n = dst->size();
    const uint8_t* mp = m->ptr();
    const uint8_t* ap = sa->ptr();
    const uint8_t* bp = sb->ptr();
    uint8_t* d = dst->ptrw();
    for (int64_t i = 0; i < n; ++i) d[i] = mp[i] ? ap[i] : bp[i];
    return zym_newNull();
}

// ---- masked arithmetic (saturating) ----

static ZymValue b_maskedAddScalar(ZymVM* vm, ZymValue ctx, ZymValue maskV, ZymValue vV) {
    PackedByteArray* m; if (!reqBuffer(vm, maskV, "Buffer.maskedAddScalar(mask, v)", &m)) return ZYM_ERROR;
    int64_t v; if (!reqInt(vm, vV, "Buffer.maskedAddScalar(mask, v)", &v)) return ZYM_ERROR;
    auto* dst = unwrap(ctx);
    if (!reqSameSize(vm, dst, m, "Buffer.maskedAddScalar(mask, v)", "mask")) return ZYM_ERROR;
    int dv = (int)v;
    int64_t n = dst->size();
    const uint8_t* mp = m->ptr();
    uint8_t* d = dst->ptrw();
    for (int64_t i = 0; i < n; ++i) if (mp[i]) d[i] = sat_add_u8(d[i], dv);
    return zym_newNull();
}

static ZymValue b_maskedSubScalar(ZymVM* vm, ZymValue ctx, ZymValue maskV, ZymValue vV) {
    PackedByteArray* m; if (!reqBuffer(vm, maskV, "Buffer.maskedSubScalar(mask, v)", &m)) return ZYM_ERROR;
    int64_t v; if (!reqInt(vm, vV, "Buffer.maskedSubScalar(mask, v)", &v)) return ZYM_ERROR;
    auto* dst = unwrap(ctx);
    if (!reqSameSize(vm, dst, m, "Buffer.maskedSubScalar(mask, v)", "mask")) return ZYM_ERROR;
    int dv = (int)v;
    int64_t n = dst->size();
    const uint8_t* mp = m->ptr();
    uint8_t* d = dst->ptrw();
    for (int64_t i = 0; i < n; ++i) if (mp[i]) d[i] = sat_sub_u8(d[i], dv);
    return zym_newNull();
}

static ZymValue b_maskedAddBuffer(ZymVM* vm, ZymValue ctx, ZymValue maskV, ZymValue srcV) {
    PackedByteArray* m; if (!reqBuffer(vm, maskV, "Buffer.maskedAddBuffer(mask, src)", &m)) return ZYM_ERROR;
    PackedByteArray* s; if (!reqBuffer(vm, srcV,  "Buffer.maskedAddBuffer(mask, src)", &s)) return ZYM_ERROR;
    auto* dst = unwrap(ctx);
    if (!reqSameSize(vm, dst, m, "Buffer.maskedAddBuffer(mask, src)", "mask")) return ZYM_ERROR;
    if (!reqSameSize(vm, dst, s, "Buffer.maskedAddBuffer(mask, src)", "src"))  return ZYM_ERROR;
    int64_t n = dst->size();
    const uint8_t* mp = m->ptr();
    const uint8_t* sp = s->ptr();
    uint8_t* d = dst->ptrw();
    for (int64_t i = 0; i < n; ++i) if (mp[i]) d[i] = sat_add_u8(d[i], sp[i]);
    return zym_newNull();
}

static ZymValue b_maskedSubBuffer(ZymVM* vm, ZymValue ctx, ZymValue maskV, ZymValue srcV) {
    PackedByteArray* m; if (!reqBuffer(vm, maskV, "Buffer.maskedSubBuffer(mask, src)", &m)) return ZYM_ERROR;
    PackedByteArray* s; if (!reqBuffer(vm, srcV,  "Buffer.maskedSubBuffer(mask, src)", &s)) return ZYM_ERROR;
    auto* dst = unwrap(ctx);
    if (!reqSameSize(vm, dst, m, "Buffer.maskedSubBuffer(mask, src)", "mask")) return ZYM_ERROR;
    if (!reqSameSize(vm, dst, s, "Buffer.maskedSubBuffer(mask, src)", "src"))  return ZYM_ERROR;
    int64_t n = dst->size();
    const uint8_t* mp = m->ptr();
    const uint8_t* sp = s->ptr();
    uint8_t* d = dst->ptrw();
    for (int64_t i = 0; i < n; ++i) if (mp[i]) d[i] = sat_sub_u8(d[i], sp[i]);
    return zym_newNull();
}

static ZymValue b_maskedAddNoise(ZymVM* vm, ZymValue ctx, ZymValue maskV, ZymValue loV, ZymValue hiV) {
    PackedByteArray* m; if (!reqBuffer(vm, maskV, "Buffer.maskedAddNoise(mask, lo, hi)", &m)) return ZYM_ERROR;
    int64_t lo, hi;
    if (!reqInt(vm, loV, "Buffer.maskedAddNoise(mask, lo, hi)", &lo)) return ZYM_ERROR;
    if (!reqInt(vm, hiV, "Buffer.maskedAddNoise(mask, lo, hi)", &hi)) return ZYM_ERROR;
    auto* dst = unwrap(ctx);
    if (!reqSameSize(vm, dst, m, "Buffer.maskedAddNoise(mask, lo, hi)", "mask")) return ZYM_ERROR;
    if (lo > hi) {
        zym_runtimeError(vm, "Buffer.maskedAddNoise(mask, lo, hi): lo (%lld) must be <= hi (%lld)",
                         (long long)lo, (long long)hi);
        return ZYM_ERROR;
    }
    int ilo = (int)lo, ihi = (int)hi;
    int64_t n = dst->size();
    const uint8_t* mp = m->ptr();
    uint8_t* d = dst->ptrw();
    for (int64_t i = 0; i < n; ++i) {
        if (mp[i]) {
            int r = Math::random(ilo, ihi);
            d[i] = sat_add_u8(d[i], r);
        }
    }
    return zym_newNull();
}

// ---- bulk arithmetic (saturating) ----

static ZymValue b_addScalar(ZymVM* vm, ZymValue ctx, ZymValue vV) {
    int64_t v; if (!reqInt(vm, vV, "Buffer.addScalar(v)", &v)) return ZYM_ERROR;
    auto* dst = unwrap(ctx);
    int dv = (int)v;
    int64_t n = dst->size();
    uint8_t* d = dst->ptrw();
    for (int64_t i = 0; i < n; ++i) d[i] = sat_add_u8(d[i], dv);
    return zym_newNull();
}

static ZymValue b_subScalar(ZymVM* vm, ZymValue ctx, ZymValue vV) {
    int64_t v; if (!reqInt(vm, vV, "Buffer.subScalar(v)", &v)) return ZYM_ERROR;
    auto* dst = unwrap(ctx);
    int dv = (int)v;
    int64_t n = dst->size();
    uint8_t* d = dst->ptrw();
    for (int64_t i = 0; i < n; ++i) d[i] = sat_sub_u8(d[i], dv);
    return zym_newNull();
}

static ZymValue b_addBuffer(ZymVM* vm, ZymValue ctx, ZymValue otherV) {
    PackedByteArray* o; if (!reqBuffer(vm, otherV, "Buffer.addBuffer(other)", &o)) return ZYM_ERROR;
    auto* dst = unwrap(ctx);
    if (!reqSameSize(vm, dst, o, "Buffer.addBuffer(other)", "other")) return ZYM_ERROR;
    int64_t n = dst->size();
    const uint8_t* s = o->ptr();
    uint8_t* d = dst->ptrw();
    for (int64_t i = 0; i < n; ++i) d[i] = sat_add_u8(d[i], s[i]);
    return zym_newNull();
}

static ZymValue b_subBuffer(ZymVM* vm, ZymValue ctx, ZymValue otherV) {
    PackedByteArray* o; if (!reqBuffer(vm, otherV, "Buffer.subBuffer(other)", &o)) return ZYM_ERROR;
    auto* dst = unwrap(ctx);
    if (!reqSameSize(vm, dst, o, "Buffer.subBuffer(other)", "other")) return ZYM_ERROR;
    int64_t n = dst->size();
    const uint8_t* s = o->ptr();
    uint8_t* d = dst->ptrw();
    for (int64_t i = 0; i < n; ++i) d[i] = sat_sub_u8(d[i], s[i]);
    return zym_newNull();
}

static ZymValue b_clampRange(ZymVM* vm, ZymValue ctx, ZymValue loV, ZymValue hiV) {
    int64_t lo, hi;
    if (!reqInt(vm, loV, "Buffer.clampRange(lo, hi)", &lo)) return ZYM_ERROR;
    if (!reqInt(vm, hiV, "Buffer.clampRange(lo, hi)", &hi)) return ZYM_ERROR;
    if (lo > hi) {
        zym_runtimeError(vm, "Buffer.clampRange(lo, hi): lo (%lld) must be <= hi (%lld)",
                         (long long)lo, (long long)hi);
        return ZYM_ERROR;
    }
    auto* dst = unwrap(ctx);
    uint8_t l = (uint8_t)(lo < 0 ? 0 : lo > 255 ? 255 : lo);
    uint8_t h = (uint8_t)(hi < 0 ? 0 : hi > 255 ? 255 : hi);
    int64_t n = dst->size();
    uint8_t* d = dst->ptrw();
    for (int64_t i = 0; i < n; ++i) {
        if (d[i] < l) d[i] = l;
        else if (d[i] > h) d[i] = h;
    }
    return zym_newNull();
}

// ---- bulk fill / copy ----

static ZymValue b_fillRandom(ZymVM* vm, ZymValue ctx, ZymValue loV, ZymValue hiV) {
    int64_t lo, hi;
    if (!reqInt(vm, loV, "Buffer.fillRandom(lo, hi)", &lo)) return ZYM_ERROR;
    if (!reqInt(vm, hiV, "Buffer.fillRandom(lo, hi)", &hi)) return ZYM_ERROR;
    if (lo < 0 || hi > 255 || lo > hi) {
        zym_runtimeError(vm, "Buffer.fillRandom(lo, hi): require 0 <= lo <= hi <= 255 (got lo=%lld, hi=%lld)",
                         (long long)lo, (long long)hi);
        return ZYM_ERROR;
    }
    auto* dst = unwrap(ctx);
    int ilo = (int)lo, ihi = (int)hi;
    int64_t n = dst->size();
    uint8_t* d = dst->ptrw();
    for (int64_t i = 0; i < n; ++i) d[i] = (uint8_t)Math::random(ilo, ihi);
    return zym_newNull();
}

static ZymValue b_copyFrom(ZymVM* vm, ZymValue ctx, ZymValue srcV) {
    PackedByteArray* s; if (!reqBuffer(vm, srcV, "Buffer.copyFrom(src)", &s)) return ZYM_ERROR;
    auto* dst = unwrap(ctx);
    if (!reqSameSize(vm, dst, s, "Buffer.copyFrom(src)", "src")) return ZYM_ERROR;
    if (dst != s && dst->size() > 0) memcpy(dst->ptrw(), s->ptr(), (size_t)dst->size());
    return zym_newNull();
}

// Intra-buffer block move. Uses memmove so overlapping ranges are well-defined.
static ZymValue b_copyRange(ZymVM* vm, ZymValue ctx, ZymValue srcOffV, ZymValue dstOffV, ZymValue lenV) {
    int64_t srcOff, dstOff, len;
    if (!reqInt(vm, srcOffV, "Buffer.copyRange(srcOffset, dstOffset, len)", &srcOff)) return ZYM_ERROR;
    if (!reqInt(vm, dstOffV, "Buffer.copyRange(srcOffset, dstOffset, len)", &dstOff)) return ZYM_ERROR;
    if (!reqInt(vm, lenV,    "Buffer.copyRange(srcOffset, dstOffset, len)", &len))    return ZYM_ERROR;
    if (len < 0) {
        zym_runtimeError(vm, "Buffer.copyRange: len must be >= 0 (got %lld)", (long long)len);
        return ZYM_ERROR;
    }
    auto* dst = unwrap(ctx);
    int64_t n = dst->size();
    if (srcOff < 0 || dstOff < 0 || srcOff + len > n || dstOff + len > n) {
        zym_runtimeError(vm, "Buffer.copyRange: range out of bounds (size=%lld, srcOff=%lld, dstOff=%lld, len=%lld)",
                         (long long)n, (long long)srcOff, (long long)dstOff, (long long)len);
        return ZYM_ERROR;
    }
    if (len > 0) memmove(dst->ptrw() + dstOff, dst->ptr() + srcOff, (size_t)len);
    return zym_newNull();
}

// Inter-buffer block copy.
static ZymValue b_blitFrom(ZymVM* vm, ZymValue ctx, ZymValue srcV, ZymValue srcOffV, ZymValue dstOffV, ZymValue lenV) {
    PackedByteArray* s; if (!reqBuffer(vm, srcV, "Buffer.blitFrom(src, srcOffset, dstOffset, len)", &s)) return ZYM_ERROR;
    int64_t srcOff, dstOff, len;
    if (!reqInt(vm, srcOffV, "Buffer.blitFrom(src, srcOffset, dstOffset, len)", &srcOff)) return ZYM_ERROR;
    if (!reqInt(vm, dstOffV, "Buffer.blitFrom(src, srcOffset, dstOffset, len)", &dstOff)) return ZYM_ERROR;
    if (!reqInt(vm, lenV,    "Buffer.blitFrom(src, srcOffset, dstOffset, len)", &len))    return ZYM_ERROR;
    if (len < 0) {
        zym_runtimeError(vm, "Buffer.blitFrom: len must be >= 0 (got %lld)", (long long)len);
        return ZYM_ERROR;
    }
    auto* dst = unwrap(ctx);
    int64_t dn = dst->size();
    int64_t sn = s->size();
    if (srcOff < 0 || dstOff < 0 || srcOff + len > sn || dstOff + len > dn) {
        zym_runtimeError(vm, "Buffer.blitFrom: range out of bounds (srcSize=%lld, dstSize=%lld, srcOff=%lld, dstOff=%lld, len=%lld)",
                         (long long)sn, (long long)dn, (long long)srcOff, (long long)dstOff, (long long)len);
        return ZYM_ERROR;
    }
    // memmove handles src == dst overlap correctly as well.
    if (len > 0) memmove(dst->ptrw() + dstOff, s->ptr() + srcOff, (size_t)len);
    return zym_newNull();
}

// Bulk copy from a Zym list of numbers into the receiver. Each list element
// is coerced to a byte (masked to 8 bits, matching `set`/`fill`/`fromList`).
// The fast path for "I have script-managed data and want to feed it into the
// bulk Buffer pipeline" — avoids the per-element `b.set(i, list[i])` loop
// that otherwise eats ~N method-dispatch costs per call site.
static ZymValue b_copyFromList(ZymVM* vm, ZymValue ctx, ZymValue listV) {
    if (!zym_isList(listV)) {
        zym_runtimeError(vm, "Buffer.copyFromList(list): expects a list");
        return ZYM_ERROR;
    }
    auto* dst = unwrap(ctx);
    int64_t n_dst  = dst->size();
    int64_t n_list = (int64_t)zym_listLength(listV);
    if (n_dst != n_list) {
        zym_runtimeError(vm, "Buffer.copyFromList(list): list length %lld does not match receiver size %lld",
                         (long long)n_list, (long long)n_dst);
        return ZYM_ERROR;
    }
    uint8_t* d = dst->ptrw();
    for (int64_t i = 0; i < n_list; i++) {
        ZymValue e = zym_listGet(vm, listV, (int)i);
        if (!zym_isNumber(e)) {
            zym_runtimeError(vm, "Buffer.copyFromList(list): element %lld is not a number", (long long)i);
            return ZYM_ERROR;
        }
        d[i] = (uint8_t)(int64_t)zym_asNumber(e);
    }
    return zym_newNull();
}

// Partial form: copy `len` elements from `list[listOffset..]` into
// `this[dstOffset..]`. Symmetric with `blitFrom` / `copyRange`. Out-of-bounds
// is a runtime error (never silent truncation).
static ZymValue b_copyFromListRange(ZymVM* vm, ZymValue ctx, ZymValue listV, ZymValue listOffV, ZymValue dstOffV, ZymValue lenV) {
    if (!zym_isList(listV)) {
        zym_runtimeError(vm, "Buffer.copyFromListRange(list, listOffset, dstOffset, len): expects a list");
        return ZYM_ERROR;
    }
    int64_t listOff, dstOff, len;
    if (!reqInt(vm, listOffV, "Buffer.copyFromListRange(list, listOffset, dstOffset, len)", &listOff)) return ZYM_ERROR;
    if (!reqInt(vm, dstOffV,  "Buffer.copyFromListRange(list, listOffset, dstOffset, len)", &dstOff))  return ZYM_ERROR;
    if (!reqInt(vm, lenV,     "Buffer.copyFromListRange(list, listOffset, dstOffset, len)", &len))     return ZYM_ERROR;
    if (len < 0) {
        zym_runtimeError(vm, "Buffer.copyFromListRange: len must be >= 0 (got %lld)", (long long)len);
        return ZYM_ERROR;
    }
    auto* dst = unwrap(ctx);
    int64_t n_dst  = dst->size();
    int64_t n_list = (int64_t)zym_listLength(listV);
    if (listOff < 0 || dstOff < 0 || listOff + len > n_list || dstOff + len > n_dst) {
        zym_runtimeError(vm, "Buffer.copyFromListRange: range out of bounds (listLen=%lld, dstSize=%lld, listOff=%lld, dstOff=%lld, len=%lld)",
                         (long long)n_list, (long long)n_dst, (long long)listOff, (long long)dstOff, (long long)len);
        return ZYM_ERROR;
    }
    uint8_t* d = dst->ptrw();
    for (int64_t i = 0; i < len; i++) {
        ZymValue e = zym_listGet(vm, listV, (int)(listOff + i));
        if (!zym_isNumber(e)) {
            zym_runtimeError(vm, "Buffer.copyFromListRange: list element %lld is not a number", (long long)(listOff + i));
            return ZYM_ERROR;
        }
        d[dstOff + i] = (uint8_t)(int64_t)zym_asNumber(e);
    }
    return zym_newNull();
}

// ---- LUT mapping ----

static ZymValue b_mapU8(ZymVM* vm, ZymValue ctx, ZymValue srcV, ZymValue lutV) {
    PackedByteArray* s; if (!reqBuffer(vm, srcV, "Buffer.mapU8(src, lut)", &s)) return ZYM_ERROR;
    PackedByteArray* l; if (!reqBuffer(vm, lutV, "Buffer.mapU8(src, lut)", &l)) return ZYM_ERROR;
    auto* dst = unwrap(ctx);
    if (!reqSameSize(vm, dst, s, "Buffer.mapU8(src, lut)", "src")) return ZYM_ERROR;
    if (l->size() < 256) {
        zym_runtimeError(vm, "Buffer.mapU8(src, lut): lut size %d must be >= 256",
                         (int)l->size());
        return ZYM_ERROR;
    }
    // src may alias dst (each read happens before the corresponding write),
    // but lut must not alias dst — writing dst[i] would mutate the LUT and
    // poison later reads.
    if (l == dst) {
        zym_runtimeError(vm, "Buffer.mapU8(src, lut): lut and receiver must not be the same Buffer");
        return ZYM_ERROR;
    }
    int64_t n = dst->size();
    const uint8_t* sp = s->ptr();
    const uint8_t* lp = l->ptr();
    uint8_t* d = dst->ptrw();
    for (int64_t i = 0; i < n; ++i) d[i] = lp[sp[i]];
    return zym_newNull();
}

// ---- decode/encode ----
// Optional trailing endian arg: "le" (default) or "be". 1-byte methods ignore it.

// Returns 0 (LE) / 1 (BE) / -1 on error. Absent arg = LE.
static int readEndian(ZymVM* vm, const char* where, ZymValue* vargs, int vargc) {
    if (vargc == 0) return 0;
    if (vargc > 1 || !zym_isString(vargs[0])) {
        zym_runtimeError(vm, "%s: optional endian arg must be \"le\" or \"be\"", where);
        return -1;
    }
    const char* s = zym_asCString(vargs[0]);
    if (s[0] == 'l' && s[1] == 'e' && s[2] == 0) return 0;
    if (s[0] == 'b' && s[1] == 'e' && s[2] == 0) return 1;
    zym_runtimeError(vm, "%s: endian must be \"le\" or \"be\"", where);
    return -1;
}

static inline uint16_t bswap_if(uint16_t v, bool be) { return be ? __builtin_bswap16(v) : v; }
static inline uint32_t bswap_if(uint32_t v, bool be) { return be ? __builtin_bswap32(v) : v; }
static inline uint64_t bswap_if(uint64_t v, bool be) { return be ? __builtin_bswap64(v) : v; }

static inline float bswap_float_if(float v, bool be) {
    if (!be) return v;
    uint32_t u; memcpy(&u, &v, 4); u = __builtin_bswap32(u); memcpy(&v, &u, 4); return v;
}
static inline double bswap_double_if(double v, bool be) {
    if (!be) return v;
    uint64_t u; memcpy(&u, &v, 8); u = __builtin_bswap64(u); memcpy(&v, &u, 8); return v;
}

// 1-byte decoders: endian arg accepted but ignored.
#define DECODE_METHOD_1(name, ctype, out_cast) \
    static ZymValue b_##name(ZymVM* vm, ZymValue ctx, ZymValue offV, ZymValue* vargs, int vargc) { \
        int64_t off; if (!reqInt(vm, offV, "Buffer." #name "(offset)", &off)) return ZYM_ERROR; \
        if (readEndian(vm, "Buffer." #name, vargs, vargc) < 0) return ZYM_ERROR; \
        auto* p = unwrap(ctx); \
        if (off < 0 || off > (int64_t)p->size() - 1) { \
            zym_runtimeError(vm, "Buffer." #name ": offset out of range"); return ZYM_ERROR; \
        } \
        return zym_newNumber((double)(out_cast)(*(const ctype*)(p->ptr() + off))); \
    }

#define DECODE_METHOD_INT(name, width, utype, stype, out_cast, signed_read) \
    static ZymValue b_##name(ZymVM* vm, ZymValue ctx, ZymValue offV, ZymValue* vargs, int vargc) { \
        int64_t off; if (!reqInt(vm, offV, "Buffer." #name "(offset)", &off)) return ZYM_ERROR; \
        int e = readEndian(vm, "Buffer." #name, vargs, vargc); if (e < 0) return ZYM_ERROR; \
        auto* p = unwrap(ctx); \
        if (off < 0 || off > (int64_t)p->size() - (width)) { \
            zym_runtimeError(vm, "Buffer." #name ": offset out of range"); return ZYM_ERROR; \
        } \
        utype raw; memcpy(&raw, p->ptr() + off, width); \
        /* PBA stores LE, so bswap when caller asked BE */ \
        raw = bswap_if(raw, e == 1); \
        if (signed_read) return zym_newNumber((double)(out_cast)(stype)raw); \
        return zym_newNumber((double)(out_cast)raw); \
    }

#define DECODE_METHOD_FLOAT(name, width, reader, swapper) \
    static ZymValue b_##name(ZymVM* vm, ZymValue ctx, ZymValue offV, ZymValue* vargs, int vargc) { \
        int64_t off; if (!reqInt(vm, offV, "Buffer." #name "(offset)", &off)) return ZYM_ERROR; \
        int e = readEndian(vm, "Buffer." #name, vargs, vargc); if (e < 0) return ZYM_ERROR; \
        auto* p = unwrap(ctx); \
        if (off < 0 || off > (int64_t)p->size() - (width)) { \
            zym_runtimeError(vm, "Buffer." #name ": offset out of range"); return ZYM_ERROR; \
        } \
        auto v = reader(p->ptr() + off); \
        return zym_newNumber((double)swapper(v, e == 1)); \
    }

// Half is a 16-bit IEEE-754 stored little-endian; swap the 2 raw bytes for BE.
static ZymValue b_decodeHalf(ZymVM* vm, ZymValue ctx, ZymValue offV, ZymValue* vargs, int vargc) {
    int64_t off; if (!reqInt(vm, offV, "Buffer.decodeHalf(offset)", &off)) return ZYM_ERROR;
    int e = readEndian(vm, "Buffer.decodeHalf", vargs, vargc); if (e < 0) return ZYM_ERROR;
    auto* p = unwrap(ctx);
    if (off < 0 || off > (int64_t)p->size() - 2) {
        zym_runtimeError(vm, "Buffer.decodeHalf: offset out of range"); return ZYM_ERROR;
    }
    uint8_t tmp[2] = { p->ptr()[off], p->ptr()[off + 1] };
    if (e == 1) { uint8_t t = tmp[0]; tmp[0] = tmp[1]; tmp[1] = t; }
    return zym_newNumber((double)decode_half(tmp));
}

DECODE_METHOD_1(decodeU8, uint8_t, uint64_t)
DECODE_METHOD_1(decodeI8, int8_t,  int64_t)
DECODE_METHOD_INT(decodeU16, 2, uint16_t, int16_t, uint64_t, false)
DECODE_METHOD_INT(decodeI16, 2, uint16_t, int16_t, int64_t,  true)
DECODE_METHOD_INT(decodeU32, 4, uint32_t, int32_t, uint64_t, false)
DECODE_METHOD_INT(decodeI32, 4, uint32_t, int32_t, int64_t,  true)
DECODE_METHOD_INT(decodeU64, 8, uint64_t, int64_t, int64_t,  false)
DECODE_METHOD_INT(decodeI64, 8, uint64_t, int64_t, int64_t,  true)
DECODE_METHOD_FLOAT(decodeFloat,  4, decode_float,  bswap_float_if)
DECODE_METHOD_FLOAT(decodeDouble, 8, decode_double, bswap_double_if)

#undef DECODE_METHOD_1
#undef DECODE_METHOD_INT
#undef DECODE_METHOD_FLOAT

// 1-byte encoders: endian arg accepted but ignored.
#define ENCODE_METHOD_1(name, ctype) \
    static ZymValue b_##name(ZymVM* vm, ZymValue ctx, ZymValue offV, ZymValue valV, ZymValue* vargs, int vargc) { \
        int64_t off; double val; \
        if (!reqInt(vm, offV, "Buffer." #name "(offset, value)", &off)) return ZYM_ERROR; \
        if (!reqNum(vm, valV, "Buffer." #name "(offset, value)", &val)) return ZYM_ERROR; \
        if (readEndian(vm, "Buffer." #name, vargs, vargc) < 0) return ZYM_ERROR; \
        auto* p = unwrap(ctx); \
        if (off < 0 || off > (int64_t)p->size() - 1) { \
            zym_runtimeError(vm, "Buffer." #name ": offset out of range"); return ZYM_ERROR; \
        } \
        p->ptrw()[off] = (uint8_t)(ctype)val; \
        return zym_newNull(); \
    }

#define ENCODE_METHOD_INT(name, width, utype, cast) \
    static ZymValue b_##name(ZymVM* vm, ZymValue ctx, ZymValue offV, ZymValue valV, ZymValue* vargs, int vargc) { \
        int64_t off; double val; \
        if (!reqInt(vm, offV, "Buffer." #name "(offset, value)", &off)) return ZYM_ERROR; \
        if (!reqNum(vm, valV, "Buffer." #name "(offset, value)", &val)) return ZYM_ERROR; \
        int e = readEndian(vm, "Buffer." #name, vargs, vargc); if (e < 0) return ZYM_ERROR; \
        auto* p = unwrap(ctx); \
        if (off < 0 || off > (int64_t)p->size() - (width)) { \
            zym_runtimeError(vm, "Buffer." #name ": offset out of range"); return ZYM_ERROR; \
        } \
        utype raw = (utype)(cast)val; \
        raw = bswap_if(raw, e == 1); \
        memcpy(p->ptrw() + off, &raw, width); \
        return zym_newNull(); \
    }

#define ENCODE_METHOD_FLOAT(name, width, writer, ftype, swapper) \
    static ZymValue b_##name(ZymVM* vm, ZymValue ctx, ZymValue offV, ZymValue valV, ZymValue* vargs, int vargc) { \
        int64_t off; double val; \
        if (!reqInt(vm, offV, "Buffer." #name "(offset, value)", &off)) return ZYM_ERROR; \
        if (!reqNum(vm, valV, "Buffer." #name "(offset, value)", &val)) return ZYM_ERROR; \
        int e = readEndian(vm, "Buffer." #name, vargs, vargc); if (e < 0) return ZYM_ERROR; \
        auto* p = unwrap(ctx); \
        if (off < 0 || off > (int64_t)p->size() - (width)) { \
            zym_runtimeError(vm, "Buffer." #name ": offset out of range"); return ZYM_ERROR; \
        } \
        writer(swapper((ftype)val, e == 1), p->ptrw() + off); \
        return zym_newNull(); \
    }

// Half: encode LE via encode_half, then byte-swap the 2 bytes in place for BE.
static ZymValue b_encodeHalf(ZymVM* vm, ZymValue ctx, ZymValue offV, ZymValue valV, ZymValue* vargs, int vargc) {
    int64_t off; double val;
    if (!reqInt(vm, offV, "Buffer.encodeHalf(offset, value)", &off)) return ZYM_ERROR;
    if (!reqNum(vm, valV, "Buffer.encodeHalf(offset, value)", &val)) return ZYM_ERROR;
    int e = readEndian(vm, "Buffer.encodeHalf", vargs, vargc); if (e < 0) return ZYM_ERROR;
    auto* p = unwrap(ctx);
    if (off < 0 || off > (int64_t)p->size() - 2) {
        zym_runtimeError(vm, "Buffer.encodeHalf: offset out of range"); return ZYM_ERROR;
    }
    uint8_t* w = p->ptrw() + off;
    encode_half((float)val, w);
    if (e == 1) { uint8_t t = w[0]; w[0] = w[1]; w[1] = t; }
    return zym_newNull();
}

ENCODE_METHOD_1(encodeU8, uint8_t)
ENCODE_METHOD_1(encodeI8, int8_t)
ENCODE_METHOD_INT(encodeU16, 2, uint16_t, uint16_t)
ENCODE_METHOD_INT(encodeI16, 2, uint16_t, int16_t)
ENCODE_METHOD_INT(encodeU32, 4, uint32_t, uint32_t)
ENCODE_METHOD_INT(encodeI32, 4, uint32_t, int32_t)
ENCODE_METHOD_INT(encodeU64, 8, uint64_t, uint64_t)
ENCODE_METHOD_INT(encodeI64, 8, uint64_t, int64_t)
ENCODE_METHOD_FLOAT(encodeFloat,  4, encode_float,  float,  bswap_float_if)
ENCODE_METHOD_FLOAT(encodeDouble, 8, encode_double, double, bswap_double_if)

#undef ENCODE_METHOD_1
#undef ENCODE_METHOD_INT
#undef ENCODE_METHOD_FLOAT

// ---- compression ----
//
// Algorithms map directly onto Godot's Compression::Mode. Beyond Godot's
// hardcoded defaults we expose an optional `level` argument:
//   "fastlz"  -> no level
//   "deflate" -> 1..9 (zlib); default Z_DEFAULT_COMPRESSION (6-equivalent)
//   "gzip"    -> 1..9 (zlib); default Z_DEFAULT_COMPRESSION (6-equivalent)
//   "zstd"    -> 1..22; default 3
//   "brotli"  -> decompress only (compress always fails in Godot core)

// Returns true on success and writes mode + level metadata.
// `out_min_level` / `out_max_level` are inclusive; -1 means "level not accepted".
static bool parseCompressAlgo(ZymVM* vm, const char* where, ZymValue algoV,
                              Compression::Mode* out_mode,
                              int* out_min_level, int* out_max_level,
                              bool* out_can_compress) {
    String s;
    if (!reqString(vm, algoV, where, &s)) return false;
    String a = s.to_lower();
    if (a == "fastlz")  { *out_mode = Compression::MODE_FASTLZ;  *out_min_level = -1; *out_max_level = -1; *out_can_compress = true;  return true; }
    if (a == "deflate") { *out_mode = Compression::MODE_DEFLATE; *out_min_level =  1; *out_max_level =  9; *out_can_compress = true;  return true; }
    if (a == "gzip")    { *out_mode = Compression::MODE_GZIP;    *out_min_level =  1; *out_max_level =  9; *out_can_compress = true;  return true; }
    if (a == "zstd")    { *out_mode = Compression::MODE_ZSTD;    *out_min_level =  1; *out_max_level = 22; *out_can_compress = true;  return true; }
    if (a == "brotli")  { *out_mode = Compression::MODE_BROTLI;  *out_min_level = -1; *out_max_level = -1; *out_can_compress = false; return true; }
    zym_runtimeError(vm, "%s: unknown algorithm \"%s\" (expected \"fastlz\", \"deflate\", \"gzip\", \"zstd\", or \"brotli\")",
                     where, s.utf8().get_data());
    return false;
}

static ZymValue b_compress(ZymVM* vm, ZymValue ctx, ZymValue algoV, ZymValue* vargs, int vargc) {
    Compression::Mode mode;
    int minL, maxL; bool canCompress;
    if (!parseCompressAlgo(vm, "Buffer.compress(algo, level?)", algoV,
                           &mode, &minL, &maxL, &canCompress)) return ZYM_ERROR;
    if (!canCompress) {
        zym_runtimeError(vm, "Buffer.compress(algo, level?): \"brotli\" is decompress-only");
        return ZYM_ERROR;
    }
    if (vargc > 1) {
        zym_runtimeError(vm, "Buffer.compress(algo, level?): too many arguments");
        return ZYM_ERROR;
    }

    int level = -1; // sentinel: "no level given, use Godot defaults"
    if (vargc == 1) {
        int64_t lv;
        if (!reqInt(vm, vargs[0], "Buffer.compress(algo, level?)", &lv)) return ZYM_ERROR;
        if (minL < 0) {
            zym_runtimeError(vm, "Buffer.compress(algo, level?): \"%s\" does not accept a level",
                             zym_asCString(algoV));
            return ZYM_ERROR;
        }
        if (lv < minL || lv > maxL) {
            zym_runtimeError(vm, "Buffer.compress(algo, level?): level %lld out of range for \"%s\" (%d..%d)",
                             (long long)lv, zym_asCString(algoV), minL, maxL);
            return ZYM_ERROR;
        }
        level = (int)lv;
    }

    // Apply level to Godot's static config for this call. Save/restore so we
    // don't disturb other code paths (FileAccessCompressed, exporters, etc.).
    int saved_zlib_level  = Compression::zlib_level;
    int saved_gzip_level  = Compression::gzip_level;
    int saved_zstd_level  = Compression::zstd_level;
    if (level >= 0) {
        switch (mode) {
            case Compression::MODE_DEFLATE: Compression::zlib_level = level; break;
            case Compression::MODE_GZIP:    Compression::gzip_level = level; break;
            case Compression::MODE_ZSTD:    Compression::zstd_level = level; break;
            default: break;
        }
    }

    auto* src = unwrap(ctx);
    int64_t src_size = (int64_t)src->size();

    PackedByteArray out;
    if (src_size == 0) {
        // Compressing empty input: most algos produce a valid empty/header-only
        // stream. Use the engine's bound API.
        int64_t bound = Compression::get_max_compressed_buffer_size(0, mode);
        if (bound < 0) {
            Compression::zlib_level = saved_zlib_level;
            Compression::gzip_level = saved_gzip_level;
            Compression::zstd_level = saved_zstd_level;
            return zym_newNull();
        }
        out.resize(bound);
        int64_t got = Compression::compress(out.ptrw(), src->ptr(), 0, mode);
        Compression::zlib_level = saved_zlib_level;
        Compression::gzip_level = saved_gzip_level;
        Compression::zstd_level = saved_zstd_level;
        if (got < 0) return zym_newNull();
        out.resize(got);
        return makeInstance(vm, out);
    }

    int64_t bound = Compression::get_max_compressed_buffer_size(src_size, mode);
    if (bound < 0) {
        Compression::zlib_level = saved_zlib_level;
        Compression::gzip_level = saved_gzip_level;
        Compression::zstd_level = saved_zstd_level;
        return zym_newNull();
    }
    out.resize(bound);
    int64_t got = Compression::compress(out.ptrw(), src->ptr(), src_size, mode);

    Compression::zlib_level = saved_zlib_level;
    Compression::gzip_level = saved_gzip_level;
    Compression::zstd_level = saved_zstd_level;

    if (got < 0) return zym_newNull();
    out.resize(got);
    return makeInstance(vm, out);
}

static ZymValue b_decompress(ZymVM* vm, ZymValue ctx, ZymValue algoV, ZymValue maxV) {
    Compression::Mode mode;
    int minL, maxL; bool canCompress;
    if (!parseCompressAlgo(vm, "Buffer.decompress(algo, maxOutputSize)", algoV,
                           &mode, &minL, &maxL, &canCompress)) return ZYM_ERROR;
    int64_t maxOut;
    if (!reqInt(vm, maxV, "Buffer.decompress(algo, maxOutputSize)", &maxOut)) return ZYM_ERROR;
    if (maxOut < 0) {
        zym_runtimeError(vm, "Buffer.decompress(algo, maxOutputSize): maxOutputSize must be >= 0");
        return ZYM_ERROR;
    }

    auto* src = unwrap(ctx);
    int64_t src_size = (int64_t)src->size();

    // gzip / deflate / brotli support streaming via decompress_dynamic, which
    // grows the output as needed up to maxOut. fastlz / zstd require a single-
    // shot decompress with a pre-sized destination, so we allocate maxOut and
    // shrink to fit afterwards.
    PackedByteArray out;
    if (mode == Compression::MODE_GZIP || mode == Compression::MODE_DEFLATE || mode == Compression::MODE_BROTLI) {
        if (src_size == 0) {
            // decompress_dynamic rejects empty input. Treat as empty result.
            return makeInstance(vm, out);
        }
        Vector<uint8_t> tmp;
        int rc = Compression::decompress_dynamic(&tmp, maxOut, src->ptr(), src_size, mode);
        if (rc != OK) return zym_newNull();
        out.resize(tmp.size());
        if (tmp.size() > 0) memcpy(out.ptrw(), tmp.ptr(), tmp.size());
        return makeInstance(vm, out);
    }

    // fastlz / zstd: pre-size to maxOut, single-shot, then shrink.
    if (maxOut == 0) return makeInstance(vm, out);
    out.resize(maxOut);
    int64_t got = Compression::decompress(out.ptrw(), maxOut, src->ptr(), src_size, mode);
    if (got < 0) return zym_newNull();
    out.resize(got);
    return makeInstance(vm, out);
}

// Streaming-style decompress that grows its own output buffer. Slower than
// `decompress` (multiple full copies as the buffer grows) but doesn't require
// the caller to know the decompressed size ahead of time. Only the algorithms
// whose underlying decoder supports streaming are accepted: gzip, deflate,
// brotli. fastlz and zstd are rejected with a runtime error.
//
// `maxOutputSize` is optional. If omitted (0 args), the decompression is
// unbounded (-1). If supplied, it must be >= 0; passing 0 yields an empty
// Buffer. Exceeding the cap mid-stream returns null (the underlying engine
// reports Z_BUF_ERROR).
static ZymValue b_decompressDynamic(ZymVM* vm, ZymValue ctx, ZymValue algoV, ZymValue* vargs, int vargc) {
    Compression::Mode mode;
    int minL, maxL; bool canCompress;
    if (!parseCompressAlgo(vm, "Buffer.decompressDynamic(algo, maxOutputSize?)", algoV,
                           &mode, &minL, &maxL, &canCompress)) return ZYM_ERROR;
    if (mode != Compression::MODE_GZIP && mode != Compression::MODE_DEFLATE && mode != Compression::MODE_BROTLI) {
        zym_runtimeError(vm, "Buffer.decompressDynamic(algo, maxOutputSize?): \"%s\" does not support dynamic decompression (use decompress instead)",
                         zym_asCString(algoV));
        return ZYM_ERROR;
    }
    if (vargc > 1) {
        zym_runtimeError(vm, "Buffer.decompressDynamic(algo, maxOutputSize?): too many arguments");
        return ZYM_ERROR;
    }

    int64_t maxOut = -1; // sentinel: unbounded
    if (vargc == 1) {
        if (!reqInt(vm, vargs[0], "Buffer.decompressDynamic(algo, maxOutputSize?)", &maxOut)) return ZYM_ERROR;
        if (maxOut < 0) {
            zym_runtimeError(vm, "Buffer.decompressDynamic(algo, maxOutputSize?): maxOutputSize must be >= 0");
            return ZYM_ERROR;
        }
    }

    auto* src = unwrap(ctx);
    int64_t src_size = (int64_t)src->size();

    PackedByteArray out;
    if (src_size == 0) {
        // decompress_dynamic rejects empty input; surface that as an empty result.
        return makeInstance(vm, out);
    }

    Vector<uint8_t> tmp;
    int rc = Compression::decompress_dynamic(&tmp, maxOut, src->ptr(), src_size, mode);
    if (rc != OK) return zym_newNull();
    out.resize(tmp.size());
    if (tmp.size() > 0) memcpy(out.ptrw(), tmp.ptr(), tmp.size());
    return makeInstance(vm, out);
}

// ---- instance assembly ----

static ZymValue makeInstance(ZymVM* vm, const PackedByteArray& src) {
    auto* data = new PackedByteArray(src);
    ZymValue ctx = zym_createNativeContext(vm, data, bufFinalizer);
    zym_pushRoot(vm, ctx);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__pba__", ctx);

#define M(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctx); \
        zym_pushRoot(vm, cl); \
        zym_mapSet(vm, obj, name, cl); \
        zym_popRoot(vm); \
    } while (0)

#define MV(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosureVariadic(vm, sig, (void*)fn, ctx); \
        zym_pushRoot(vm, cl); \
        zym_mapSet(vm, obj, name, cl); \
        zym_popRoot(vm); \
    } while (0)

    M("size",      "size()",              b_size);
    M("isEmpty",   "isEmpty()",           b_isEmpty);
    M("clear",     "clear()",             b_clear);
    M("resize",    "resize(n)",           b_resize);
    M("fill",      "fill(v)",             b_fill);
    M("duplicate", "duplicate()",         b_duplicate);
    M("get",       "get(i)",              b_get);
    M("set",       "set(i, v)",           b_set);
    M("append",    "append(v)",           b_append);
    M("pushBack",  "pushBack(v)",         b_append);
    M("insert",    "insert(i, v)",        b_insert);
    M("removeAt",  "removeAt(i)",         b_removeAt);
    M("erase",     "erase(v)",            b_erase);
    M("reverse",   "reverse()",           b_reverse);
    M("sort",      "sort()",              b_sort);
    M("has",       "has(v)",              b_has);
    M("find",      "find(v, from)",       b_find);
    M("rfind",     "rfind(v, from)",      b_rfind);
    M("count",     "count(v)",            b_count);
    M("bsearch",   "bsearch(v, before)",  b_bsearch);
    M("slice",     "slice(begin, end)",   b_slice);
    M("equals",    "equals(other)",       b_equals);
    M("concat",    "concat(other)",       b_concat);
    M("hex",       "hex()",               b_hex);
    M("toUtf8",    "toUtf8()",            b_toUtf8);
    M("toAscii",   "toAscii()",           b_toAscii);

    // ---- predicates (mask construction) ----
    M("eqScalar",  "eqScalar(src, v)",          b_eqScalar);
    M("neqScalar", "neqScalar(src, v)",         b_neqScalar);
    M("ltScalar",  "ltScalar(src, v)",          b_ltScalar);
    M("leScalar",  "leScalar(src, v)",          b_leScalar);
    M("gtScalar",  "gtScalar(src, v)",          b_gtScalar);
    M("geScalar",  "geScalar(src, v)",          b_geScalar);
    M("inRange",   "inRange(src, lo, hi)",      b_inRange);
    M("eqBuffer",  "eqBuffer(srcA, srcB)",      b_eqBuffer);
    M("neqBuffer", "neqBuffer(srcA, srcB)",     b_neqBuffer);
    M("ltBuffer",  "ltBuffer(srcA, srcB)",      b_ltBuffer);
    M("leBuffer",  "leBuffer(srcA, srcB)",      b_leBuffer);
    M("gtBuffer",  "gtBuffer(srcA, srcB)",      b_gtBuffer);
    M("geBuffer",  "geBuffer(srcA, srcB)",      b_geBuffer);

    // ---- bitwise (per-byte) ----
    M("bitAnd",       "bitAnd(other)",        b_bitAnd);
    M("bitOr",        "bitOr(other)",         b_bitOr);
    M("bitXor",       "bitXor(other)",        b_bitXor);
    M("bitNot",       "bitNot()",             b_bitNot);
    M("bitAndScalar", "bitAndScalar(v)",      b_bitAndScalar);
    M("bitOrScalar",  "bitOrScalar(v)",       b_bitOrScalar);
    M("bitXorScalar", "bitXorScalar(v)",      b_bitXorScalar);

    // ---- reductions ----
    M("countNonZero", "countNonZero()",       b_countNonZero);
    M("any",          "any()",                b_any);
    M("all",          "all()",                b_all);

    // ---- masked writes ----
    M("maskedFill", "maskedFill(mask, v)",          b_maskedFill);
    M("maskedCopy", "maskedCopy(mask, src)",        b_maskedCopy);
    M("select",     "select(mask, srcA, srcB)",     b_select);

    // ---- masked arithmetic (saturating) ----
    M("maskedAddScalar", "maskedAddScalar(mask, v)",      b_maskedAddScalar);
    M("maskedSubScalar", "maskedSubScalar(mask, v)",      b_maskedSubScalar);
    M("maskedAddBuffer", "maskedAddBuffer(mask, src)",    b_maskedAddBuffer);
    M("maskedSubBuffer", "maskedSubBuffer(mask, src)",    b_maskedSubBuffer);
    M("maskedAddNoise",  "maskedAddNoise(mask, lo, hi)",  b_maskedAddNoise);

    // ---- bulk arithmetic (saturating) ----
    M("addScalar",  "addScalar(v)",         b_addScalar);
    M("subScalar",  "subScalar(v)",         b_subScalar);
    M("addBuffer",  "addBuffer(other)",     b_addBuffer);
    M("subBuffer",  "subBuffer(other)",     b_subBuffer);
    M("clampRange", "clampRange(lo, hi)",   b_clampRange);

    // ---- bulk fill / copy ----
    M("fillRandom", "fillRandom(lo, hi)",                          b_fillRandom);
    M("copyFrom",          "copyFrom(src)",                                              b_copyFrom);
    M("copyRange",         "copyRange(srcOffset, dstOffset, len)",                       b_copyRange);
    M("blitFrom",          "blitFrom(src, srcOffset, dstOffset, len)",                   b_blitFrom);
    M("copyFromList",      "copyFromList(list)",                                         b_copyFromList);
    M("copyFromListRange", "copyFromListRange(list, listOffset, dstOffset, len)",        b_copyFromListRange);

    // ---- LUT mapping ----
    M("mapU8", "mapU8(src, lut)", b_mapU8);

    MV("compress",         "compress(algo, ...)",                         b_compress);
    M ("decompress",       "decompress(algo, maxOutputSize)",             b_decompress);
    MV("decompressDynamic","decompressDynamic(algo, ...)",                b_decompressDynamic);

    MV("decodeU8",     "decodeU8(offset, ...)",     b_decodeU8);
    MV("decodeI8",     "decodeI8(offset, ...)",     b_decodeI8);
    MV("decodeU16",    "decodeU16(offset, ...)",    b_decodeU16);
    MV("decodeI16",    "decodeI16(offset, ...)",    b_decodeI16);
    MV("decodeU32",    "decodeU32(offset, ...)",    b_decodeU32);
    MV("decodeI32",    "decodeI32(offset, ...)",    b_decodeI32);
    MV("decodeU64",    "decodeU64(offset, ...)",    b_decodeU64);
    MV("decodeI64",    "decodeI64(offset, ...)",    b_decodeI64);
    MV("decodeHalf",   "decodeHalf(offset, ...)",   b_decodeHalf);
    MV("decodeFloat",  "decodeFloat(offset, ...)",  b_decodeFloat);
    MV("decodeDouble", "decodeDouble(offset, ...)", b_decodeDouble);

    MV("encodeU8",     "encodeU8(offset, value, ...)",     b_encodeU8);
    MV("encodeI8",     "encodeI8(offset, value, ...)",     b_encodeI8);
    MV("encodeU16",    "encodeU16(offset, value, ...)",    b_encodeU16);
    MV("encodeI16",    "encodeI16(offset, value, ...)",    b_encodeI16);
    MV("encodeU32",    "encodeU32(offset, value, ...)",    b_encodeU32);
    MV("encodeI32",    "encodeI32(offset, value, ...)",    b_encodeI32);
    MV("encodeU64",    "encodeU64(offset, value, ...)",    b_encodeU64);
    MV("encodeI64",    "encodeI64(offset, value, ...)",    b_encodeI64);
    MV("encodeHalf",   "encodeHalf(offset, value, ...)",   b_encodeHalf);
    MV("encodeFloat",  "encodeFloat(offset, value, ...)",  b_encodeFloat);
    MV("encodeDouble", "encodeDouble(offset, value, ...)", b_encodeDouble);

#undef M
#undef MV

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}

// ---- Buffer global (constructors) ----

static ZymValue c_new(ZymVM* vm, ZymValue, ZymValue sizeV) {
    int64_t n; if (!reqInt(vm, sizeV, "Buffer.new(size)", &n)) return ZYM_ERROR;
    if (n < 0) { zym_runtimeError(vm, "Buffer.new(size): size must be >= 0"); return ZYM_ERROR; }
    PackedByteArray pba;
    pba.resize(n);
    if (n > 0) memset(pba.ptrw(), 0, (size_t)n);
    return makeInstance(vm, pba);
}

static ZymValue c_fromBytes(ZymVM* vm, ZymValue, ZymValue other) {
    PackedByteArray* o; if (!reqBuffer(vm, other, "Buffer.fromBytes(buf)", &o)) return ZYM_ERROR;
    return makeInstance(vm, *o);
}

static ZymValue c_fromHex(ZymVM* vm, ZymValue, ZymValue sv) {
    String s; if (!reqString(vm, sv, "Buffer.fromHex(s)", &s)) return ZYM_ERROR;
    if (s.length() % 2 != 0) { zym_runtimeError(vm, "Buffer.fromHex(s): odd-length string"); return ZYM_ERROR; }
    PackedByteArray pba;
    pba.resize(s.length() / 2);
    uint8_t* w = pba.ptrw();
    auto hexNibble = [](char32_t c) -> int {
        if (c >= '0' && c <= '9') return (int)(c - '0');
        if (c >= 'a' && c <= 'f') return (int)(c - 'a') + 10;
        if (c >= 'A' && c <= 'F') return (int)(c - 'A') + 10;
        return -1;
    };
    for (int i = 0; i < s.length(); i += 2) {
        int hi = hexNibble((char32_t)s[i]);
        int lo = hexNibble((char32_t)s[i + 1]);
        if (hi < 0 || lo < 0) { zym_runtimeError(vm, "Buffer.fromHex(s): non-hex character"); return ZYM_ERROR; }
        w[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    return makeInstance(vm, pba);
}

static ZymValue c_fromString(ZymVM* vm, ZymValue, ZymValue sv) {
    if (!zym_isString(sv)) { zym_runtimeError(vm, "Buffer.fromString(s): expects a string"); return ZYM_ERROR; }
    const char* bytes = nullptr; int len = 0;
    zym_toStringBytes(sv, &bytes, &len);
    PackedByteArray pba;
    if (len > 0) {
        pba.resize(len);
        memcpy(pba.ptrw(), bytes, (size_t)len);
    }
    return makeInstance(vm, pba);
}

static ZymValue c_fromList(ZymVM* vm, ZymValue, ZymValue listV) {
    if (!zym_isList(listV)) { zym_runtimeError(vm, "Buffer.fromList(list): expects a list"); return ZYM_ERROR; }
    int n = zym_listLength(listV);
    PackedByteArray pba;
    pba.resize(n);
    uint8_t* w = pba.ptrw();
    for (int i = 0; i < n; i++) {
        ZymValue e = zym_listGet(vm, listV, i);
        if (!zym_isNumber(e)) { zym_runtimeError(vm, "Buffer.fromList(list): element %d is not a number", i); return ZYM_ERROR; }
        w[i] = (uint8_t)(int64_t)zym_asNumber(e);
    }
    return makeInstance(vm, pba);
}

ZymValue nativeBuffer_create(ZymVM* vm) {
    ZymValue ctx = zym_createNativeContext(vm, nullptr, nullptr);
    zym_pushRoot(vm, ctx);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

#define CTOR(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctx); \
        zym_pushRoot(vm, cl); \
        zym_mapSet(vm, obj, name, cl); \
        zym_popRoot(vm); \
    } while (0)

    CTOR("new",        "new(size)",        c_new);
    CTOR("fromBytes",  "fromBytes(buf)",   c_fromBytes);
    CTOR("fromHex",    "fromHex(s)",       c_fromHex);
    CTOR("fromString", "fromString(s)",    c_fromString);
    CTOR("fromList",   "fromList(list)",   c_fromList);

#undef CTOR

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}
