// `Zym` native module — see zym_native.hpp for scope.
//
// This PR ships one working method (`cliNatives`) so the per-VM
// `ZymCliVmCtx` wiring is exercised by an actual script-visible call.
// Everything else from roadmap §2 lands in subsequent PRs against the
// shape this file establishes (one per-VM context capturing the
// calling VM's `ZymCliVmCtx*`, methods read state via
// `zym_getNativeData(context)`).

#include "zym_native.hpp"

#include "../cli_catalog.hpp"

namespace {

// Methods receive `context` as their first argument; we stash the
// calling VM's `ZymCliVmCtx*` in the context's userdata so any method
// can fetch it in one indirection without going through the
// process-wide map.
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

ZymValue nativeZym_create(ZymVM* vm) {
    // Bind every method to a context whose userdata is the calling
    // VM's CLI ctx. The ctx is owned by `cli_catalog`'s side table
    // (and freed by the auto-attached finalizer there); we *don't*
    // pass a finalizer here because the lifetime is not ours to
    // manage — this context is just a lookup handle.
    ZymCliVmCtx* cli = cli_catalog_ctx(vm);
    ZymValue context = zym_createNativeContext(vm, cli, nullptr);
    zym_pushRoot(vm, context);

    ZymValue cliNatives = zym_createNativeClosure(vm, "cliNatives()",
                                                  (void*)z_cliNatives, context);
    zym_pushRoot(vm, cliNatives);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

    zym_mapSet(vm, obj, "cliNatives", cliNatives);

    // context + cliNatives + obj = 3
    for (int i = 0; i < 3; i++) zym_popRoot(vm);
    return obj;
}
