// Godot-backed RegEx / RegExMatch native.
//
// Per-instance shape: each compiled pattern and each match result is a
// map-of-closures bound to a context whose native data is a Ref<T>* (heap-
// allocated, deleted by the finalizer). Tag keys disambiguate handle types:
//
//   "__re__"   -> Ref<RegEx>*       (RegEx instance)
//   "__rem__"  -> Ref<RegExMatch>*  (RegExMatch instance)
//
// `RegEx` global exposes the constructor `create(pattern)`. Compiled regexes
// expose `compile`, `clear`, `search`, `searchAll`, `sub`, `isValid`,
// `pattern`, `groupCount`, and `names`. Match instances expose `subject`,
// `groupCount`, `names`, `strings`, `string`, `start`, `end` -- with
// `string` / `start` / `end` accepting either a numeric group index or a
// named-capture string, matching Godot's Variant-typed API.
#include "core/string/ustring.h"
#include "core/templates/vector.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "core/variant/typed_array.h"
#include "modules/regex/regex.h"

#include "natives.hpp"

// ---- helpers ----

struct ReHandle    { Ref<RegEx>      r; };
struct ReMatchHandle { Ref<RegExMatch> m; };

static void reFinalizer(ZymVM*, void* data) {
    delete static_cast<ReHandle*>(data);
}
static void remFinalizer(ZymVM*, void* data) {
    delete static_cast<ReMatchHandle*>(data);
}

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

// Resolve an optional integer in a variadic tail. If absent, leaves *out
// untouched and returns 0; on present-but-bad-type returns -1 (with error
// already raised); on success returns 1.
static int optInt(ZymVM* vm, const char* where, ZymValue* vargs, int vargc, int idx, int64_t* out) {
    if (idx >= vargc) return 0;
    if (!zym_isNumber(vargs[idx])) {
        zym_runtimeError(vm, "%s: argument %d must be a number", where, idx + 1);
        return -1;
    }
    *out = (int64_t)zym_asNumber(vargs[idx]);
    return 1;
}

// Coerce a ZymValue to a Variant suitable for RegExMatch's name-or-index
// accessors. Strings -> String; numbers -> int. Anything else is an error.
static bool toNameOrIndex(ZymVM* vm, ZymValue v, const char* where, Variant* out) {
    if (zym_isNumber(v)) { *out = (int)zym_asNumber(v); return true; }
    if (zym_isString(v)) { *out = String::utf8(zym_asCString(v)); return true; }
    zym_runtimeError(vm, "%s: name must be a number (group index) or string (named group)", where);
    return false;
}

static ReHandle* unwrapRE(ZymValue ctx) {
    return static_cast<ReHandle*>(zym_getNativeData(ctx));
}
static ReMatchHandle* unwrapREM(ZymValue ctx) {
    return static_cast<ReMatchHandle*>(zym_getNativeData(ctx));
}

// Forward decls.
static ZymValue makeRegExInstance(ZymVM* vm, Ref<RegEx> r);
static ZymValue makeRegExMatchInstance(ZymVM* vm, Ref<RegExMatch> m);

// ---- RegExMatch instance methods ----

static ZymValue m_subject(ZymVM* vm, ZymValue ctx) {
    ReMatchHandle* h = unwrapREM(ctx);
    if (!h || h->m.is_null()) return zym_newString(vm, "");
    return strZ(vm, h->m->get_subject());
}

static ZymValue m_groupCount(ZymVM* vm, ZymValue ctx) {
    ReMatchHandle* h = unwrapREM(ctx);
    if (!h || h->m.is_null()) return zym_newNumber(0);
    return zym_newNumber((double)h->m->get_group_count());
}

static ZymValue m_names(ZymVM* vm, ZymValue ctx) {
    ZymValue out = zym_newMap(vm);
    ReMatchHandle* h = unwrapREM(ctx);
    if (!h || h->m.is_null()) return out;
    Dictionary names = h->m->get_names();
    zym_pushRoot(vm, out);
    Array keys = names.keys();
    for (int i = 0; i < keys.size(); i++) {
        String k = keys[i];
        int idx = (int)names[keys[i]];
        CharString ku = k.utf8();
        zym_mapSet(vm, out, ku.get_data(), zym_newNumber((double)idx));
    }
    zym_popRoot(vm);
    return out;
}

static ZymValue m_strings(ZymVM* vm, ZymValue ctx) {
    ZymValue out = zym_newList(vm);
    ReMatchHandle* h = unwrapREM(ctx);
    if (!h || h->m.is_null()) return out;
    PackedStringArray ss = h->m->get_strings();
    zym_pushRoot(vm, out);
    for (int i = 0; i < ss.size(); i++) {
        zym_listAppend(vm, out, strZ(vm, ss[i]));
    }
    zym_popRoot(vm);
    return out;
}

static ZymValue m_string(ZymVM* vm, ZymValue ctx, ZymValue nameV) {
    ReMatchHandle* h = unwrapREM(ctx);
    if (!h || h->m.is_null()) { zym_runtimeError(vm, "RegExMatch.string(name): match is invalid"); return ZYM_ERROR; }
    Variant key;
    if (!toNameOrIndex(vm, nameV, "RegExMatch.string(name)", &key)) return ZYM_ERROR;
    return strZ(vm, h->m->get_string(key));
}

static ZymValue m_start(ZymVM* vm, ZymValue ctx, ZymValue nameV) {
    ReMatchHandle* h = unwrapREM(ctx);
    if (!h || h->m.is_null()) { zym_runtimeError(vm, "RegExMatch.start(name): match is invalid"); return ZYM_ERROR; }
    Variant key;
    if (!toNameOrIndex(vm, nameV, "RegExMatch.start(name)", &key)) return ZYM_ERROR;
    return zym_newNumber((double)h->m->get_start(key));
}

static ZymValue m_end(ZymVM* vm, ZymValue ctx, ZymValue nameV) {
    ReMatchHandle* h = unwrapREM(ctx);
    if (!h || h->m.is_null()) { zym_runtimeError(vm, "RegExMatch.end(name): match is invalid"); return ZYM_ERROR; }
    Variant key;
    if (!toNameOrIndex(vm, nameV, "RegExMatch.end(name)", &key)) return ZYM_ERROR;
    return zym_newNumber((double)h->m->get_end(key));
}

// ---- RegEx instance methods ----

static ZymValue r_isValid(ZymVM*, ZymValue ctx) {
    ReHandle* h = unwrapRE(ctx);
    return zym_newBool(h != nullptr && h->r.is_valid() && h->r->is_valid());
}

static ZymValue r_pattern(ZymVM* vm, ZymValue ctx) {
    ReHandle* h = unwrapRE(ctx);
    if (!h || h->r.is_null()) return zym_newString(vm, "");
    return strZ(vm, h->r->get_pattern());
}

static ZymValue r_groupCount(ZymVM* vm, ZymValue ctx) {
    ReHandle* h = unwrapRE(ctx);
    if (!h || h->r.is_null() || !h->r->is_valid()) return zym_newNumber(0);
    return zym_newNumber((double)h->r->get_group_count());
}

static ZymValue r_names(ZymVM* vm, ZymValue ctx) {
    ZymValue out = zym_newList(vm);
    ReHandle* h = unwrapRE(ctx);
    if (!h || h->r.is_null() || !h->r->is_valid()) return out;
    PackedStringArray ns = h->r->get_names();
    zym_pushRoot(vm, out);
    for (int i = 0; i < ns.size(); i++) {
        zym_listAppend(vm, out, strZ(vm, ns[i]));
    }
    zym_popRoot(vm);
    return out;
}

static ZymValue r_clear(ZymVM*, ZymValue ctx) {
    ReHandle* h = unwrapRE(ctx);
    if (h && h->r.is_valid()) h->r->clear();
    return zym_newNull();
}

static ZymValue r_compile(ZymVM* vm, ZymValue ctx, ZymValue patV) {
    ReHandle* h = unwrapRE(ctx);
    if (!h || h->r.is_null()) { zym_runtimeError(vm, "RegEx.compile(pattern): handle is invalid"); return ZYM_ERROR; }
    String pat; if (!reqStr(vm, patV, "RegEx.compile(pattern)", &pat)) return ZYM_ERROR;
    return zym_newBool(h->r->compile(pat, /*p_show_error=*/false) == OK);
}

static ZymValue r_search(ZymVM* vm, ZymValue ctx, ZymValue subjV, ZymValue* vargs, int vargc) {
    ReHandle* h = unwrapRE(ctx);
    if (!h || h->r.is_null() || !h->r->is_valid()) { zym_runtimeError(vm, "RegEx.search(subject, ...): pattern is not compiled"); return ZYM_ERROR; }
    String subject; if (!reqStr(vm, subjV, "RegEx.search(subject, ...)", &subject)) return ZYM_ERROR;
    int64_t offset = 0;
    int64_t end = -1;
    if (optInt(vm, "RegEx.search(subject, ...)", vargs, vargc, 0, &offset) < 0) return ZYM_ERROR;
    if (optInt(vm, "RegEx.search(subject, ...)", vargs, vargc, 1, &end)    < 0) return ZYM_ERROR;
    Ref<RegExMatch> m = h->r->search(subject, (int)offset, (int)end);
    if (m.is_null()) return zym_newNull();
    return makeRegExMatchInstance(vm, m);
}

static ZymValue r_searchAll(ZymVM* vm, ZymValue ctx, ZymValue subjV, ZymValue* vargs, int vargc) {
    ZymValue out = zym_newList(vm);
    ReHandle* h = unwrapRE(ctx);
    if (!h || h->r.is_null() || !h->r->is_valid()) { zym_runtimeError(vm, "RegEx.searchAll(subject, ...): pattern is not compiled"); return ZYM_ERROR; }
    String subject; if (!reqStr(vm, subjV, "RegEx.searchAll(subject, ...)", &subject)) return ZYM_ERROR;
    int64_t offset = 0;
    int64_t end = -1;
    if (optInt(vm, "RegEx.searchAll(subject, ...)", vargs, vargc, 0, &offset) < 0) return ZYM_ERROR;
    if (optInt(vm, "RegEx.searchAll(subject, ...)", vargs, vargc, 1, &end)    < 0) return ZYM_ERROR;
    TypedArray<RegExMatch> arr = h->r->search_all(subject, (int)offset, (int)end);
    zym_pushRoot(vm, out);
    for (int i = 0; i < arr.size(); i++) {
        Ref<RegExMatch> m = arr[i];
        if (m.is_null()) continue;
        ZymValue inst = makeRegExMatchInstance(vm, m);
        zym_pushRoot(vm, inst);
        zym_listAppend(vm, out, inst);
        zym_popRoot(vm);
    }
    zym_popRoot(vm);
    return out;
}

static ZymValue r_sub(ZymVM* vm, ZymValue ctx, ZymValue subjV, ZymValue replV, ZymValue* vargs, int vargc) {
    ReHandle* h = unwrapRE(ctx);
    if (!h || h->r.is_null() || !h->r->is_valid()) { zym_runtimeError(vm, "RegEx.sub(subject, replacement, ...): pattern is not compiled"); return ZYM_ERROR; }
    String subject; if (!reqStr(vm, subjV, "RegEx.sub(subject, replacement, ...)", &subject)) return ZYM_ERROR;
    String repl;    if (!reqStr(vm, replV, "RegEx.sub(subject, replacement, ...)", &repl))    return ZYM_ERROR;
    bool all = false;
    int64_t offset = 0;
    int64_t end = -1;
    if (vargc > 0) {
        if (!reqBool(vm, vargs[0], "RegEx.sub(subject, replacement, ...)", &all)) return ZYM_ERROR;
    }
    if (optInt(vm, "RegEx.sub(subject, replacement, ...)", vargs, vargc, 1, &offset) < 0) return ZYM_ERROR;
    if (optInt(vm, "RegEx.sub(subject, replacement, ...)", vargs, vargc, 2, &end)    < 0) return ZYM_ERROR;
    return strZ(vm, h->r->sub(subject, repl, all, (int)offset, (int)end));
}

// ---- instance assembly ----

static ZymValue makeRegExMatchInstance(ZymVM* vm, Ref<RegExMatch> m) {
    auto* data = new ReMatchHandle{ m };
    ZymValue ctx = zym_createNativeContext(vm, data, remFinalizer);
    zym_pushRoot(vm, ctx);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__rem__", ctx);

#define M(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctx); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)

    M("subject",     "subject()",      m_subject);
    M("groupCount",  "groupCount()",   m_groupCount);
    M("names",       "names()",        m_names);
    M("strings",     "strings()",      m_strings);
    M("string",      "string(name)",   m_string);
    M("start",       "start(name)",    m_start);
    M("end",         "end(name)",      m_end);

#undef M

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}

static ZymValue makeRegExInstance(ZymVM* vm, Ref<RegEx> r) {
    auto* data = new ReHandle{ r };
    ZymValue ctx = zym_createNativeContext(vm, data, reFinalizer);
    zym_pushRoot(vm, ctx);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);
    zym_mapSet(vm, obj, "__re__", ctx);

#define M(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctx); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)
#define MV(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosureVariadic(vm, sig, (void*)fn, ctx); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)

    M("isValid",     "isValid()",          r_isValid);
    M("pattern",     "pattern()",          r_pattern);
    M("groupCount",  "groupCount()",       r_groupCount);
    M("names",       "names()",            r_names);
    M("clear",       "clear()",            r_clear);
    M("compile",     "compile(pattern)",   r_compile);
    MV("search",     "search(subject, ...)",     r_search);
    MV("searchAll",  "searchAll(subject, ...)",  r_searchAll);
    MV("sub",        "sub(subject, replacement, ...)", r_sub);

#undef M
#undef MV

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}

// ---- RegEx global (statics) ----

static ZymValue f_create(ZymVM* vm, ZymValue, ZymValue patV) {
    String pat; if (!reqStr(vm, patV, "RegEx.create(pattern)", &pat)) return ZYM_ERROR;
    Ref<RegEx> r = RegEx::create_from_string(pat, /*p_show_error=*/false);
    if (r.is_null() || !r->is_valid()) return zym_newNull();
    return makeRegExInstance(vm, r);
}

static ZymValue f_empty(ZymVM* vm, ZymValue) {
    Ref<RegEx> r;
    r.instantiate();
    return makeRegExInstance(vm, r);
}

// ---- factory ----

ZymValue nativeRegex_create(ZymVM* vm) {
    ZymValue ctx = zym_createNativeContext(vm, nullptr, nullptr);
    zym_pushRoot(vm, ctx);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

#define F(name, sig, fn) do { \
        ZymValue cl = zym_createNativeClosure(vm, sig, (void*)fn, ctx); \
        zym_pushRoot(vm, cl); zym_mapSet(vm, obj, name, cl); zym_popRoot(vm); \
    } while (0)

    F("create", "create(pattern)", f_create);
    F("empty",  "empty()",         f_empty);

#undef F

    zym_popRoot(vm); // obj
    zym_popRoot(vm); // ctx
    return obj;
}
