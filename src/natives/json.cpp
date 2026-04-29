// JSON native: zym-flavored API over Godot's `core/io/json.h` (PCRE2-free).
//
// Surface:
//   JSON.stringify(value, indent?, sortKeys?, fullPrecision?)  -> string
//   JSON.parse(text)                                           -> value | null
//   JSON.create()                                              -> JSON instance
//
// Instance (returned by `JSON.create()`):
//   parse(text)                  -> bool   (true = OK)
//   parse(text, keepText)        -> bool
//   data()                       -> last successfully parsed value (or null)
//   setData(value)               -> null
//   parsedText()                 -> string (only populated if keepText=true)
//   errorLine()                  -> number (1-based; 0 when no error)
//   errorMessage()               -> string ("" when no error)
//
// Conversions (zym maps are string-key only):
//   zym null/bool/number/string/list/map  <->  Variant NIL/BOOL/FLOAT/STRING/ARRAY/DICTIONARY
//   Numbers always round-trip as FLOAT on the way in; integral JSON numbers
//   come back as zym numbers (doubles), matching how zym treats numbers.
//   Dictionary keys with non-string types coming back from JSON are coerced
//   to strings via Variant->String for safety; in practice JSON parse always
//   produces String keys, so this is just a guard.
//
// Cycle / depth handling: zym->Variant conversion bails out at depth 512 with
// a runtime error. Variant->zym never recurses through cycles because parsed
// JSON values are acyclic by construction.
#include "core/io/json.h"
#include "core/string/ustring.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

#include "natives.hpp"

// ---- handle ----

struct JsonHandle { Ref<JSON> j; };

static void jsonFinalizer(ZymVM*, void* data) {
    delete static_cast<JsonHandle*>(data);
}

static JsonHandle* unwrapJSON(ZymValue ctx) {
    return static_cast<JsonHandle*>(zym_getNativeData(ctx));
}

// ---- small helpers ----

static ZymValue strZ(ZymVM* vm, const String& s) {
    CharString u = s.utf8();
    return zym_newStringN(vm, u.get_data(), u.length());
}

static bool reqStr(ZymVM* vm, ZymValue v, const char* where, String* out) {
    if (!zym_isString(v)) { zym_runtimeError(vm, "%s expects a string", where); return false; }
    *out = String::utf8(zym_asCString(v)); return true;
}
static bool reqBool(ZymVM* vm, ZymValue v, const char* where, bool* out) {
    if (!zym_isBool(v)) { zym_runtimeError(vm, "%s expects a bool", where); return false; }
    *out = zym_asBool(v); return true;
}

// ---- ZymValue -> Variant ----

static constexpr int kJsonMaxDepth = 512;

static bool zymToVariant(ZymVM* vm, ZymValue v, int depth, Variant* out);

struct MapPackCtx {
    ZymVM* vm;
    Dictionary* dict;
    int depth;
    bool ok;
};

static bool mapPackIter(ZymVM* vm, const char* key, ZymValue val, void* userdata) {
    MapPackCtx* c = static_cast<MapPackCtx*>(userdata);
    if (!c->ok) return false;
    Variant nested;
    if (!zymToVariant(vm, val, c->depth + 1, &nested)) { c->ok = false; return false; }
    (*c->dict)[String::utf8(key)] = nested;
    return true;
}

static bool zymToVariant(ZymVM* vm, ZymValue v, int depth, Variant* out) {
    if (depth > kJsonMaxDepth) {
        zym_runtimeError(vm, "JSON: value nests deeper than %d levels", kJsonMaxDepth);
        return false;
    }
    if (zym_isNull(v))   { *out = Variant();              return true; }
    if (zym_isBool(v))   { *out = zym_asBool(v);          return true; }
    if (zym_isNumber(v)) { *out = zym_asNumber(v);        return true; }
    if (zym_isString(v)) { *out = String::utf8(zym_asCString(v)); return true; }
    if (zym_isList(v)) {
        Array arr;
        int n = zym_listLength(v);
        arr.resize(n);
        for (int i = 0; i < n; i++) {
            ZymValue elem = zym_listGet(vm, v, i);
            Variant nested;
            if (!zymToVariant(vm, elem, depth + 1, &nested)) return false;
            arr[i] = nested;
        }
        *out = arr;
        return true;
    }
    if (zym_isMap(v)) {
        Dictionary dict;
        MapPackCtx ctx{ vm, &dict, depth, true };
        zym_mapForEach(vm, v, mapPackIter, &ctx);
        if (!ctx.ok) return false;
        *out = dict;
        return true;
    }
    zym_runtimeError(vm, "JSON: cannot encode value of type '%s'", zym_typeName(v));
    return false;
}

// ---- Variant -> ZymValue ----

static ZymValue variantToZym(ZymVM* vm, const Variant& v) {
    switch (v.get_type()) {
        case Variant::NIL:
            return zym_newNull();
        case Variant::BOOL:
            return zym_newBool((bool)v);
        case Variant::INT:
            return zym_newNumber((double)(int64_t)v);
        case Variant::FLOAT:
            return zym_newNumber((double)v);
        case Variant::STRING:
        case Variant::STRING_NAME:
            return strZ(vm, (String)v);
        case Variant::ARRAY: {
            Array arr = v;
            ZymValue out = zym_newList(vm);
            zym_pushRoot(vm, out);
            for (int i = 0; i < arr.size(); i++) {
                ZymValue elem = variantToZym(vm, arr[i]);
                zym_pushRoot(vm, elem);
                zym_listAppend(vm, out, elem);
                zym_popRoot(vm);
            }
            zym_popRoot(vm);
            return out;
        }
        case Variant::DICTIONARY: {
            Dictionary dict = v;
            ZymValue out = zym_newMap(vm);
            zym_pushRoot(vm, out);
            Array keys = dict.keys();
            for (int i = 0; i < keys.size(); i++) {
                String k = keys[i]; // coerces non-string keys
                CharString ku = k.utf8();
                ZymValue val = variantToZym(vm, dict[keys[i]]);
                zym_pushRoot(vm, val);
                zym_mapSet(vm, out, ku.get_data(), val);
                zym_popRoot(vm);
            }
            zym_popRoot(vm);
            return out;
        }
        default:
            // Anything else (Object, Vector2/3, Color, ...) won't appear from
            // pure JSON parse output, but render to string as a fallback.
            return strZ(vm, v.stringify());
    }
}

// ---- forward decl ----

static ZymValue makeJsonInstance(ZymVM* vm, Ref<JSON> j);

// ---- JSON instance methods ----

static ZymValue j_parse(ZymVM* vm, ZymValue ctx, ZymValue textV, ZymValue* vargs, int vargc) {
    JsonHandle* h = unwrapJSON(ctx);
    if (!h || h->j.is_null()) { zym_runtimeError(vm, "JSON.parse(text, ...): handle is invalid"); return ZYM_ERROR; }
    String text;   if (!reqStr(vm, textV, "JSON.parse(text, ...)", &text)) return ZYM_ERROR;
    bool keepText = false;
    if (vargc > 0) { if (!reqBool(vm, vargs[0], "JSON.parse(text, ...)", &keepText)) return ZYM_ERROR; }
    Error err = h->j->parse(text, keepText);
    return zym_newBool(err == OK);
}

static ZymValue j_data(ZymVM* vm, ZymValue ctx) {
    JsonHandle* h = unwrapJSON(ctx);
    if (!h || h->j.is_null()) return zym_newNull();
    return variantToZym(vm, h->j->get_data());
}

static ZymValue j_setData(ZymVM* vm, ZymValue ctx, ZymValue v) {
    JsonHandle* h = unwrapJSON(ctx);
    if (!h || h->j.is_null()) { zym_runtimeError(vm, "JSON.setData(value): handle is invalid"); return ZYM_ERROR; }
    Variant var;
    if (!zymToVariant(vm, v, 0, &var)) return ZYM_ERROR;
    h->j->set_data(var);
    return zym_newNull();
}

static ZymValue j_parsedText(ZymVM* vm, ZymValue ctx) {
    JsonHandle* h = unwrapJSON(ctx);
    if (!h || h->j.is_null()) return zym_newString(vm, "");
    return strZ(vm, h->j->get_parsed_text());
}

static ZymValue j_errorLine(ZymVM*, ZymValue ctx) {
    JsonHandle* h = unwrapJSON(ctx);
    if (!h || h->j.is_null()) return zym_newNumber(0);
    return zym_newNumber((double)h->j->get_error_line());
}

static ZymValue j_errorMessage(ZymVM* vm, ZymValue ctx) {
    JsonHandle* h = unwrapJSON(ctx);
    if (!h || h->j.is_null()) return zym_newString(vm, "");
    return strZ(vm, h->j->get_error_message());
}

// ---- instance assembly ----

static ZymValue makeJsonInstance(ZymVM* vm, Ref<JSON> j) {
    auto* data = new JsonHandle{ j };
    ZymValue ctx = zym_createNativeContext(vm, data, jsonFinalizer);
    zym_pushRoot(vm, ctx);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__json__", ctx);

#define M(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctx); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)
#define MV(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosureVariadic(vm, sig, (void*)fn, ctx); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)

    MV("parse",        "parse(text, ...)",   j_parse);
    M("data",          "data()",             j_data);
    M("setData",       "setData(value)",     j_setData);
    M("parsedText",    "parsedText()",       j_parsedText);
    M("errorLine",     "errorLine()",        j_errorLine);
    M("errorMessage",  "errorMessage()",     j_errorMessage);

#undef M
#undef MV

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}

// ---- JSON global statics ----

static ZymValue f_create(ZymVM* vm, ZymValue) {
    Ref<JSON> j;
    j.instantiate();
    return makeJsonInstance(vm, j);
}

static ZymValue f_stringify(ZymVM* vm, ZymValue, ZymValue valV, ZymValue* vargs, int vargc) {
    Variant var;
    if (!zymToVariant(vm, valV, 0, &var)) return ZYM_ERROR;
    String indent = "";
    bool sortKeys = true;
    bool fullPrecision = false;
    if (vargc > 0) { if (!reqStr(vm, vargs[0],  "JSON.stringify(value, ...)", &indent))        return ZYM_ERROR; }
    if (vargc > 1) { if (!reqBool(vm, vargs[1], "JSON.stringify(value, ...)", &sortKeys))      return ZYM_ERROR; }
    if (vargc > 2) { if (!reqBool(vm, vargs[2], "JSON.stringify(value, ...)", &fullPrecision)) return ZYM_ERROR; }
    return strZ(vm, JSON::stringify(var, indent, sortKeys, fullPrecision));
}

static ZymValue f_parse(ZymVM* vm, ZymValue, ZymValue textV) {
    String text; if (!reqStr(vm, textV, "JSON.parse(text)", &text)) return ZYM_ERROR;
    Ref<JSON> j;
    j.instantiate();
    if (j->parse(text) != OK) return zym_newNull();
    return variantToZym(vm, j->get_data());
}

// ---- factory ----

ZymValue nativeJson_create(ZymVM* vm) {
    ZymValue ctx = zym_createNativeContext(vm, nullptr, nullptr);
    zym_pushRoot(vm, ctx);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

#define F(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctx); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)
#define FV(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosureVariadic(vm, sig, (void*)fn, ctx); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)

    F("create",      "create()",            f_create);
    FV("stringify",  "stringify(value, ...)", f_stringify);
    F("parse",       "parse(text)",         f_parse);

#undef F
#undef FV

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}
