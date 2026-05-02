// `Zym` native module — see zym_native.hpp for scope.
//
// Lifetime model (mirrors the pre-godot CLI's `VMData`/`zymvm_cleanup`
// pattern in src/natives/ZymVM.c): the per-VM `ZymCliVmCtx*` is owned
// by a single finalizer-bearing `zym_createNativeContext` value
// produced in `nativeZym_create`. Every `Zym.*` method closure shares
// that context, so the ctx pointer travels with the closures. When
// the VM tears down its globals, the closures drop, the shared
// context is GC'd, and the finalizer below frees the ctx. There is
// no process-wide bookkeeping.

#include "zym_native.hpp"

#include "../cli_catalog.hpp"

namespace {

// Finalizer fires when the shared closure context is GC'd, which
// happens when the VM's globals (which hold the `Zym` map and thus
// every method closure) are torn down. At that point no other code
// is running on this VM, so it's safe to free the ctx allocation.
void zym_ctx_finalizer(ZymVM* /*vm*/, void* p) {
    delete static_cast<ZymCliVmCtx*>(p);
}

// Methods receive `context` as their first argument; the userdata is
// the calling VM's `ZymCliVmCtx*` (set up in `nativeZym_create`
// below).
ZymValue z_cliNatives(ZymVM* vm, ZymValue context) {
    auto* cli = static_cast<ZymCliVmCtx*>(zym_getNativeData(context));
    ZymValue list = zym_newList(vm);
    if (!cli) return list;

    // Declaration-order iteration; matches roadmap §0 → Errors note
    // ("catalog declaration order, deterministic").
    for (const auto& name : cli->available) {
        zym_listAppend(vm, list, zym_newString(vm, name.c_str()));
    }
    return list;
}

} // namespace

ZymValue nativeZym_create(ZymVM* vm, ZymCliVmCtx* ctx) {
    // The shared context wraps `ctx` with the finalizer that owns
    // its lifetime. Every Zym.* method closure binds to this exact
    // value, so all methods see the same ctx and the ctx is freed
    // exactly once when all closures are gone.
    ZymValue sharedCtx = zym_createNativeContext(vm, ctx, zym_ctx_finalizer);
    zym_pushRoot(vm, sharedCtx);

    ZymValue cliNatives = zym_createNativeClosure(vm, "cliNatives()",
                                                  (void*)z_cliNatives, sharedCtx);
    zym_pushRoot(vm, cliNatives);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

    zym_mapSet(vm, obj, "cliNatives", cliNatives);

    // sharedCtx + cliNatives + obj = 3
    for (int i = 0; i < 3; i++) zym_popRoot(vm);
    return obj;
}
