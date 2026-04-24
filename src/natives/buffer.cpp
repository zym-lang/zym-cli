// Godot-backed Buffer (PackedByteArray wrapper).
// Per-instance: each buffer is a map-of-closures bound to a context whose
// native data is a `new PackedByteArray` (deleted by the finalizer).
// CoW value-semantics: mutation methods detach via ptrw() when shared.
#include "core/io/marshalls.h"
#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/variant.h"

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
    if (p->size() > 0) s.append_utf8((const char*)p->ptr(), p->size());
    return stringToZym(vm, s);
}
static ZymValue b_toAscii(ZymVM* vm, ZymValue ctx) {
    auto* p = unwrap(ctx);
    String s;
    if (p->size() > 0) s.append_ascii(Span<char>((const char*)p->ptr(), p->size()));
    return stringToZym(vm, s);
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
