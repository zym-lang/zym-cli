// Cross-VM value bridge -- see cross_vm.hpp for the design rationale.
//
// Mechanism mirror of zym-js (`src/zym_js_api.c`) adapted for two ZymVMs in
// the same process: handle table is unnecessary for value graphs (ZymValues
// are passed directly within-process), but the **callable wrapper** model is
// retained so a closure crossing the boundary becomes a fresh native closure
// on the destination VM that dispatches back into the source VM.
//
// Buffer is recognised via the existing `readBufferBytes` / `makeBufferFromBytes`
// helpers (see `src/natives/buffer.cpp`). All other map values pass through
// the generic recursive map marshaller.
#include "cross_vm.hpp"

#include "../natives/natives.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace zym_bridge {

namespace {

// =============================================================================
// Identity-tracked recursive marshaller
// =============================================================================
//
// Per-call cache: source ZymValue -> destination ZymValue. Used so a graph
// containing the same list/map/struct twice produces a single shared
// destination value, and so cycles terminate.
using IdentityMap = std::unordered_map<uint64_t, ZymValue>;

bool marshal_rec(ZymVM* srcVm, ZymVM* dstVm, ZymValue v,
                 IdentityMap& seen, ZymValue* out);

bool marshal_list(ZymVM* srcVm, ZymVM* dstVm, ZymValue list,
                  IdentityMap& seen, ZymValue* out) {
    // Anchor source list on srcVm so its storage isn't reclaimed by a
    // collection triggered by destination-side allocations during the
    // recursive walk.
    zym_pushRoot(srcVm, list);
    ZymValue dst = zym_newList(dstVm);
    if (dst == ZYM_ERROR) {
        zym_popRoot(srcVm);
        zym_runtimeError(srcVm, "cross-vm marshal: out of memory (list)");
        return false;
    }
    seen[(uint64_t)list] = dst;
    zym_pushRoot(dstVm, dst);
    int n = zym_listLength(list);
    for (int i = 0; i < n; i++) {
        ZymValue srcItem = zym_listGet(srcVm, list, i);
        if (srcItem == ZYM_ERROR) {
            zym_popRoot(dstVm);
            zym_popRoot(srcVm);
            zym_runtimeError(srcVm, "cross-vm marshal: list read failed");
            return false;
        }
        ZymValue dstItem;
        if (!marshal_rec(srcVm, dstVm, srcItem, seen, &dstItem)) {
            zym_popRoot(dstVm);
            zym_popRoot(srcVm);
            return false;
        }
        zym_listAppend(dstVm, dst, dstItem);
    }
    zym_popRoot(dstVm);
    zym_popRoot(srcVm);
    *out = dst;
    return true;
}

struct MapEntry {
    std::string key;
    ZymValue    val;
};

struct MapSnapshotCtx {
    std::vector<MapEntry>* out;
};

bool map_snapshot_each(ZymVM* /*iterVm*/, const char* key, ZymValue val, void* userdata) {
    auto* ctx = static_cast<MapSnapshotCtx*>(userdata);
    ctx->out->push_back({ key ? std::string(key) : std::string(), val });
    return true;
}

bool marshal_map(ZymVM* srcVm, ZymVM* dstVm, ZymValue map,
                 IdentityMap& seen, ZymValue* out) {
    // Buffer is a map with a hidden `__pba__` native-context entry. Recognise
    // and byte-copy it before the generic map path so the destination Buffer
    // is a fresh, independent instance.
    const char* bytes = nullptr;
    size_t size = 0;
    if (readBufferBytes(srcVm, map, &bytes, &size)) {
        ZymValue dst = makeBufferFromBytes(dstVm, bytes, size);
        if (dst == ZYM_ERROR) {
            zym_runtimeError(srcVm, "cross-vm marshal: out of memory (Buffer)");
            return false;
        }
        seen[(uint64_t)map] = dst;
        *out = dst;
        return true;
    }

    // Anchor source map on srcVm so its key storage and value handles stay
    // live across destination-side allocations during the recursive walk.
    zym_pushRoot(srcVm, map);
    ZymValue dst = zym_newMap(dstVm);
    if (dst == ZYM_ERROR) {
        zym_popRoot(srcVm);
        zym_runtimeError(srcVm, "cross-vm marshal: out of memory (map)");
        return false;
    }
    seen[(uint64_t)map] = dst;
    zym_pushRoot(dstVm, dst);

    // Two-pass: snapshot the source map's entries before allocating
    // anything else on either VM, then marshal each value over the
    // (stable) snapshot. Avoids invalidating the source map's iterator
    // when destination-side allocations trigger collection or rehash.
    std::vector<MapEntry> entries;
    {
        MapSnapshotCtx snap { &entries };
        zym_mapForEach(srcVm, map, map_snapshot_each, &snap);
    }
    bool failed = false;
    for (auto& e : entries) {
        ZymValue dstVal;
        if (!marshal_rec(srcVm, dstVm, e.val, seen, &dstVal)) {
            failed = true;
            break;
        }
        zym_mapSet(dstVm, dst, e.key.c_str(), dstVal);
    }

    zym_popRoot(dstVm);
    zym_popRoot(srcVm);
    if (failed) return false;
    *out = dst;
    return true;
}

bool marshal_struct(ZymVM* srcVm, ZymVM* dstVm, ZymValue st,
                    IdentityMap& seen, ZymValue* out) {
    const char* name = zym_structGetName(st);
    if (!name) {
        zym_runtimeError(srcVm, "cross-vm marshal: struct has no name");
        return false;
    }
    zym_pushRoot(srcVm, st);
    ZymValue dst = zym_newStruct(dstVm, name);
    if (dst == ZYM_ERROR) {
        zym_popRoot(srcVm);
        zym_runtimeError(srcVm,
            "cross-vm marshal: struct '%s' is not defined on the destination VM",
            name);
        return false;
    }
    seen[(uint64_t)st] = dst;
    zym_pushRoot(dstVm, dst);
    int n = zym_structFieldCount(st);
    for (int i = 0; i < n; i++) {
        const char* field = zym_structFieldNameAt(st, i);
        if (!field) continue;
        ZymValue srcField = zym_structGet(srcVm, st, field);
        if (srcField == ZYM_ERROR) continue;
        ZymValue dstField;
        if (!marshal_rec(srcVm, dstVm, srcField, seen, &dstField)) {
            zym_popRoot(dstVm);
            zym_popRoot(srcVm);
            return false;
        }
        zym_structSet(dstVm, dst, field, dstField);
    }
    zym_popRoot(dstVm);
    zym_popRoot(srcVm);
    *out = dst;
    return true;
}

bool marshal_enum(ZymVM* srcVm, ZymVM* dstVm, ZymValue en, ZymValue* out) {
    const char* type    = zym_enumGetName(srcVm, en);
    const char* variant = zym_enumGetVariant(srcVm, en);
    if (!type || !variant) {
        zym_runtimeError(srcVm, "cross-vm marshal: enum has no name/variant");
        return false;
    }
    ZymValue dst = zym_newEnum(dstVm, type, variant);
    if (dst == ZYM_ERROR) {
        zym_runtimeError(srcVm,
            "cross-vm marshal: enum '%s.%s' is not defined on the destination VM",
            type, variant);
        return false;
    }
    *out = dst;
    return true;
}

// Forward decl; defined after the trampoline table below.
ZymValue make_callable_wrapper(ZymVM* srcVm, ZymVM* dstVm, ZymValue srcCallable);

bool marshal_rec(ZymVM* srcVm, ZymVM* dstVm, ZymValue v,
                 IdentityMap& seen, ZymValue* out) {
    if (zym_isNull(v))   { *out = zym_newNull();                 return true; }
    if (zym_isBool(v))   { *out = zym_newBool(zym_asBool(v));    return true; }
    if (zym_isNumber(v)) { *out = zym_newNumber(zym_asNumber(v));return true; }
    if (zym_isString(v)) {
        const char* s = zym_asCString(v);
        ZymValue dst = zym_newString(dstVm, s ? s : "");
        if (dst == ZYM_ERROR) {
            zym_runtimeError(srcVm, "cross-vm marshal: out of memory (string)");
            return false;
        }
        *out = dst;
        return true;
    }

    auto it = seen.find((uint64_t)v);
    if (it != seen.end()) { *out = it->second; return true; }

    if (zym_isList(v))   return marshal_list(srcVm, dstVm, v, seen, out);
    if (zym_isMap(v))    return marshal_map(srcVm, dstVm, v, seen, out);
    if (zym_isStruct(v)) return marshal_struct(srcVm, dstVm, v, seen, out);
    if (zym_isEnum(v))   return marshal_enum(srcVm, dstVm, v, out);

    if (zym_isClosure(v) || zym_isFunction(v)) {
        ZymValue wrapped = make_callable_wrapper(srcVm, dstVm, v);
        if (wrapped == ZYM_ERROR) return false;
        seen[(uint64_t)v] = wrapped;
        *out = wrapped;
        return true;
    }

    zym_runtimeError(srcVm,
        "cross-vm marshal: value of unsupported kind cannot cross VM boundary");
    return false;
}

// =============================================================================
// Cross-VM callable shared state + trampolines
// =============================================================================
//
// Each cross-VM callable stores a CrossCallable on the destination-VM closure
// context. When the destination invokes the closure, the trampoline marshals
// args into the source VM, calls the source callable via zym_callClosurev,
// and marshals the result back.
//
// `srcVm` may be torn down before the destination closure is collected; the
// `srcVm` pointer is *only* dereferenced from inside this trampoline, which
// happens during a destination-VM call -- both VMs must exist at that moment
// or the parent script has misused the API. We don't track liveness here
// because the script-side `ChildVmHandle` already errors `no such native` on
// any operation against a freed child.

struct CrossCallable {
    ZymVM*   srcVm;
    ZymValue srcCallable;  // referenced via the GC root anchor below
};

// Anchor live cross-VM callables on a hidden global of their source VM so the
// source-VM GC keeps them alive for as long as the wrapper exists. Each
// CrossCallable allocates one slot in this map; the finalizer removes it.
constexpr const char* kAnchorMapGlobal = "__zymBridge_anchors__";

ZymValue ensure_anchor_map(ZymVM* vm) {
    // We can't query globals through the public API directly, so we keep a
    // process-wide map keyed by ZymVM* of the anchor map pointers; defining
    // the global once per VM is idempotent under last-write-wins semantics
    // anyway, but probing is cheaper.
    static std::unordered_map<ZymVM*, ZymValue> g_anchorMaps;
    auto it = g_anchorMaps.find(vm);
    if (it != g_anchorMaps.end()) return it->second;
    ZymValue m = zym_newMap(vm);
    if (m == ZYM_ERROR) return ZYM_ERROR;
    if (zym_defineGlobal(vm, kAnchorMapGlobal, m) != ZYM_STATUS_OK) {
        return ZYM_ERROR;
    }
    g_anchorMaps[vm] = m;
    return m;
}

static uint64_t g_nextAnchorId = 1;

bool anchor_callable(ZymVM* vm, ZymValue callable, std::string& out_key) {
    ZymValue map = ensure_anchor_map(vm);
    if (map == ZYM_ERROR) return false;
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)g_nextAnchorId++);
    if (!zym_mapSet(vm, map, buf, callable)) return false;
    out_key = buf;
    return true;
}

void release_anchor(ZymVM* vm, const std::string& key) {
    static std::unordered_map<ZymVM*, ZymValue> g_anchorMaps;  // local stub
    // ensure_anchor_map's static lives in its own translation; resolve the
    // map through the same path so we delete from the right table.
    ZymValue map = ensure_anchor_map(vm);
    if (map == ZYM_ERROR) return;
    zym_mapDelete(vm, map, key.c_str());
}

struct WrapperCtx {
    ZymVM*      srcVm;
    std::string anchorKey;  // anchored on srcVm under kAnchorMapGlobal
};

void wrapper_finalizer(ZymVM* /*dstVm*/, void* p) {
    auto* w = static_cast<WrapperCtx*>(p);
    if (!w) return;
    if (w->srcVm) release_anchor(w->srcVm, w->anchorKey);
    delete w;
}

ZymValue resolve_src_callable(WrapperCtx* w) {
    ZymValue map = ensure_anchor_map(w->srcVm);
    if (map == ZYM_ERROR) return ZYM_ERROR;
    return zym_mapGet(w->srcVm, map, w->anchorKey.c_str());
}

// Single dispatch helper used by every fixed/variadic trampoline.
ZymValue dispatch(ZymVM* dstVm, ZymValue ctx, int arity, const ZymValue* args,
                  bool isVariadic, const ZymValue* vargs, int vargc) {
    auto* w = static_cast<WrapperCtx*>(zym_getNativeData(ctx));
    if (!w || !w->srcVm) {
        zym_runtimeError(dstVm, "no such native");
        return ZYM_ERROR;
    }
    ZymValue srcCallable = resolve_src_callable(w);
    if (srcCallable == ZYM_ERROR || zym_isNull(srcCallable)) {
        zym_runtimeError(dstVm, "no such native");
        return ZYM_ERROR;
    }

    int totalArgc = arity + (isVariadic ? vargc : 0);
    std::vector<ZymValue> srcArgs;
    srcArgs.reserve((size_t)totalArgc);

    IdentityMap seen;
    for (int i = 0; i < arity; i++) {
        ZymValue srcArg;
        if (!marshal_rec(dstVm, w->srcVm, args[i], seen, &srcArg)) {
            zym_runtimeError(dstVm, "no such native");
            return ZYM_ERROR;
        }
        srcArgs.push_back(srcArg);
    }
    if (isVariadic) {
        for (int i = 0; i < vargc; i++) {
            ZymValue srcArg;
            if (!marshal_rec(dstVm, w->srcVm, vargs[i], seen, &srcArg)) {
                zym_runtimeError(dstVm, "no such native");
                return ZYM_ERROR;
            }
            srcArgs.push_back(srcArg);
        }
    }

    ZymStatus st = zym_callClosurev(w->srcVm, srcCallable, totalArgc,
                                    srcArgs.empty() ? nullptr : srcArgs.data());
    if (st != ZYM_STATUS_OK) {
        zym_runtimeError(dstVm, "no such native");
        return ZYM_ERROR;
    }
    ZymValue srcResult = zym_getCallResult(w->srcVm);
    IdentityMap seen2;
    ZymValue dstResult;
    if (!marshal_rec(w->srcVm, dstVm, srcResult, seen2, &dstResult)) {
        zym_runtimeError(dstVm, "no such native");
        return ZYM_ERROR;
    }
    return dstResult;
}

// -- Fixed-arity trampolines (0..10) ------------------------------------------
// Expanded by hand to avoid the comma-in-macro-args problem. Each trampoline
// builds a small ZymValue array and forwards into `dispatch`.

ZymValue ft0(ZymVM* vm, ZymValue ctx) {
    return dispatch(vm, ctx, 0, nullptr, false, nullptr, 0);
}
ZymValue ft1(ZymVM* vm, ZymValue ctx, ZymValue a0) {
    ZymValue a[] = { a0 };
    return dispatch(vm, ctx, 1, a, false, nullptr, 0);
}
ZymValue ft2(ZymVM* vm, ZymValue ctx, ZymValue a0, ZymValue a1) {
    ZymValue a[] = { a0, a1 };
    return dispatch(vm, ctx, 2, a, false, nullptr, 0);
}
ZymValue ft3(ZymVM* vm, ZymValue ctx, ZymValue a0, ZymValue a1, ZymValue a2) {
    ZymValue a[] = { a0, a1, a2 };
    return dispatch(vm, ctx, 3, a, false, nullptr, 0);
}
ZymValue ft4(ZymVM* vm, ZymValue ctx, ZymValue a0, ZymValue a1, ZymValue a2, ZymValue a3) {
    ZymValue a[] = { a0, a1, a2, a3 };
    return dispatch(vm, ctx, 4, a, false, nullptr, 0);
}
ZymValue ft5(ZymVM* vm, ZymValue ctx, ZymValue a0, ZymValue a1, ZymValue a2, ZymValue a3, ZymValue a4) {
    ZymValue a[] = { a0, a1, a2, a3, a4 };
    return dispatch(vm, ctx, 5, a, false, nullptr, 0);
}
ZymValue ft6(ZymVM* vm, ZymValue ctx, ZymValue a0, ZymValue a1, ZymValue a2, ZymValue a3, ZymValue a4, ZymValue a5) {
    ZymValue a[] = { a0, a1, a2, a3, a4, a5 };
    return dispatch(vm, ctx, 6, a, false, nullptr, 0);
}
ZymValue ft7(ZymVM* vm, ZymValue ctx, ZymValue a0, ZymValue a1, ZymValue a2, ZymValue a3, ZymValue a4, ZymValue a5, ZymValue a6) {
    ZymValue a[] = { a0, a1, a2, a3, a4, a5, a6 };
    return dispatch(vm, ctx, 7, a, false, nullptr, 0);
}
ZymValue ft8(ZymVM* vm, ZymValue ctx, ZymValue a0, ZymValue a1, ZymValue a2, ZymValue a3, ZymValue a4, ZymValue a5, ZymValue a6, ZymValue a7) {
    ZymValue a[] = { a0, a1, a2, a3, a4, a5, a6, a7 };
    return dispatch(vm, ctx, 8, a, false, nullptr, 0);
}
ZymValue ft9(ZymVM* vm, ZymValue ctx, ZymValue a0, ZymValue a1, ZymValue a2, ZymValue a3, ZymValue a4, ZymValue a5, ZymValue a6, ZymValue a7, ZymValue a8) {
    ZymValue a[] = { a0, a1, a2, a3, a4, a5, a6, a7, a8 };
    return dispatch(vm, ctx, 9, a, false, nullptr, 0);
}
ZymValue ft10(ZymVM* vm, ZymValue ctx, ZymValue a0, ZymValue a1, ZymValue a2, ZymValue a3, ZymValue a4, ZymValue a5, ZymValue a6, ZymValue a7, ZymValue a8, ZymValue a9) {
    ZymValue a[] = { a0, a1, a2, a3, a4, a5, a6, a7, a8, a9 };
    return dispatch(vm, ctx, 10, a, false, nullptr, 0);
}

void* const kFixedTramp[11] = {
    (void*)ft0,  (void*)ft1,  (void*)ft2,  (void*)ft3,
    (void*)ft4,  (void*)ft5,  (void*)ft6,  (void*)ft7,
    (void*)ft8,  (void*)ft9,  (void*)ft10,
};

// -- Variadic trampolines (0..10 fixed prefix) --------------------------------

ZymValue vt0(ZymVM* vm, ZymValue ctx, ZymValue* va, int vc) {
    return dispatch(vm, ctx, 0, nullptr, true, va, vc);
}
ZymValue vt1(ZymVM* vm, ZymValue ctx, ZymValue a0, ZymValue* va, int vc) {
    ZymValue a[] = { a0 };
    return dispatch(vm, ctx, 1, a, true, va, vc);
}
ZymValue vt2(ZymVM* vm, ZymValue ctx, ZymValue a0, ZymValue a1, ZymValue* va, int vc) {
    ZymValue a[] = { a0, a1 };
    return dispatch(vm, ctx, 2, a, true, va, vc);
}
ZymValue vt3(ZymVM* vm, ZymValue ctx, ZymValue a0, ZymValue a1, ZymValue a2, ZymValue* va, int vc) {
    ZymValue a[] = { a0, a1, a2 };
    return dispatch(vm, ctx, 3, a, true, va, vc);
}
ZymValue vt4(ZymVM* vm, ZymValue ctx, ZymValue a0, ZymValue a1, ZymValue a2, ZymValue a3, ZymValue* va, int vc) {
    ZymValue a[] = { a0, a1, a2, a3 };
    return dispatch(vm, ctx, 4, a, true, va, vc);
}
ZymValue vt5(ZymVM* vm, ZymValue ctx, ZymValue a0, ZymValue a1, ZymValue a2, ZymValue a3, ZymValue a4, ZymValue* va, int vc) {
    ZymValue a[] = { a0, a1, a2, a3, a4 };
    return dispatch(vm, ctx, 5, a, true, va, vc);
}
ZymValue vt6(ZymVM* vm, ZymValue ctx, ZymValue a0, ZymValue a1, ZymValue a2, ZymValue a3, ZymValue a4, ZymValue a5, ZymValue* va, int vc) {
    ZymValue a[] = { a0, a1, a2, a3, a4, a5 };
    return dispatch(vm, ctx, 6, a, true, va, vc);
}
ZymValue vt7(ZymVM* vm, ZymValue ctx, ZymValue a0, ZymValue a1, ZymValue a2, ZymValue a3, ZymValue a4, ZymValue a5, ZymValue a6, ZymValue* va, int vc) {
    ZymValue a[] = { a0, a1, a2, a3, a4, a5, a6 };
    return dispatch(vm, ctx, 7, a, true, va, vc);
}
ZymValue vt8(ZymVM* vm, ZymValue ctx, ZymValue a0, ZymValue a1, ZymValue a2, ZymValue a3, ZymValue a4, ZymValue a5, ZymValue a6, ZymValue a7, ZymValue* va, int vc) {
    ZymValue a[] = { a0, a1, a2, a3, a4, a5, a6, a7 };
    return dispatch(vm, ctx, 8, a, true, va, vc);
}
ZymValue vt9(ZymVM* vm, ZymValue ctx, ZymValue a0, ZymValue a1, ZymValue a2, ZymValue a3, ZymValue a4, ZymValue a5, ZymValue a6, ZymValue a7, ZymValue a8, ZymValue* va, int vc) {
    ZymValue a[] = { a0, a1, a2, a3, a4, a5, a6, a7, a8 };
    return dispatch(vm, ctx, 9, a, true, va, vc);
}
ZymValue vt10(ZymVM* vm, ZymValue ctx, ZymValue a0, ZymValue a1, ZymValue a2, ZymValue a3, ZymValue a4, ZymValue a5, ZymValue a6, ZymValue a7, ZymValue a8, ZymValue a9, ZymValue* va, int vc) {
    ZymValue a[] = { a0, a1, a2, a3, a4, a5, a6, a7, a8, a9 };
    return dispatch(vm, ctx, 10, a, true, va, vc);
}

void* const kVarTramp[11] = {
    (void*)vt0,  (void*)vt1,  (void*)vt2,  (void*)vt3,
    (void*)vt4,  (void*)vt5,  (void*)vt6,  (void*)vt7,
    (void*)vt8,  (void*)vt9,  (void*)vt10,
};

// -- Wrapper construction -----------------------------------------------------

ZymValue make_wrapper_with(ZymVM* srcVm, ZymVM* dstVm, ZymValue srcCallable,
                           const char* signature, bool variadic, int arity) {
    auto* w = new WrapperCtx{ srcVm, std::string{} };
    if (!anchor_callable(srcVm, srcCallable, w->anchorKey)) {
        delete w;
        zym_runtimeError(dstVm, "cross-vm marshal: failed to anchor source callable");
        return ZYM_ERROR;
    }
    ZymValue ctx = zym_createNativeContext(dstVm, w, wrapper_finalizer);
    if (zym_isNull(ctx)) {
        wrapper_finalizer(dstVm, w);
        zym_runtimeError(dstVm, "cross-vm marshal: failed to create native context");
        return ZYM_ERROR;
    }
    void* fp = variadic ? kVarTramp[arity] : kFixedTramp[arity];
    ZymValue closure = variadic
        ? zym_createNativeClosureVariadic(dstVm, signature, fp, ctx)
        : zym_createNativeClosure(dstVm, signature, fp, ctx);
    if (zym_isNull(closure)) {
        zym_runtimeError(dstVm, "cross-vm marshal: failed to create native closure");
        return ZYM_ERROR;
    }
    return closure;
}

ZymValue make_callable_wrapper(ZymVM* srcVm, ZymVM* dstVm, ZymValue srcCallable) {
    // No introspectable arity; wrap as variadic with zero fixed prefix.
    return make_wrapper_with(srcVm, dstVm, srcCallable,
                             "__zymBridgeCallable(...)", true, 0);
}

// -- Signature parsing --------------------------------------------------------

int parse_arity(const char* sig, bool* out_variadic, std::string* out_name) {
    if (out_variadic) *out_variadic = false;
    if (out_name) out_name->clear();
    const char* p = strchr(sig, '(');
    if (!p) return -1;
    if (out_name) out_name->assign(sig, p - sig);
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == ')') return 0;

    int count = 1;
    bool variadic = false;
    int depth = 0;
    for (; *p && !(depth == 0 && *p == ')'); p++) {
        if      (*p == '(') depth++;
        else if (*p == ')') depth--;
        else if (*p == ',' && depth == 0) count++;
        else if (*p == '.' && p[1] == '.' && p[2] == '.') variadic = true;
    }
    if (variadic) count--;
    if (out_variadic) *out_variadic = variadic;
    return count;
}

}  // namespace

// =============================================================================
// Public API
// =============================================================================

bool marshal(ZymVM* srcVm, ZymVM* dstVm, ZymValue v, ZymValue* out) {
    IdentityMap seen;
    return marshal_rec(srcVm, dstVm, v, seen, out);
}

ZymValue wrap_callable(ZymVM* srcVm, ZymVM* dstVm, ZymValue srcCallable) {
    return make_callable_wrapper(srcVm, dstVm, srcCallable);
}

bool register_cross_native(ZymVM* parentVm, ZymVM* childVm,
                           const char* signature, ZymValue parentClosure) {
    bool variadic = false;
    std::string name;
    int arity = parse_arity(signature, &variadic, &name);
    if (arity < 0 || arity > 10) {
        zym_runtimeError(parentVm,
            "registerNative(sig, fn): bad signature '%s'", signature);
        return false;
    }
    if (!zym_isClosure(parentClosure) && !zym_isFunction(parentClosure)) {
        zym_runtimeError(parentVm,
            "registerNative(sig, fn): fn must be a closure");
        return false;
    }
    ZymValue closure = make_wrapper_with(parentVm, childVm, parentClosure,
                                         signature, variadic, arity);
    if (closure == ZYM_ERROR) {
        zym_runtimeError(parentVm,
            "registerNative(sig, fn): failed to wrap parent closure");
        return false;
    }
    ZymValue bound = closure;
    if (zym_defineGlobal(childVm, name.c_str(), bound) != ZYM_STATUS_OK) {
        zym_runtimeError(parentVm,
            "registerNative(sig, fn): failed to bind '%s' on child", name.c_str());
        return false;
    }
    return true;
}

}  // namespace zym_bridge
