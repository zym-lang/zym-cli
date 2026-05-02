// `Zym` native module — see zym_native.hpp for scope.
//
// Lifetime model (mirrors the pre-godot CLI's `VMData`/`zymvm_cleanup`
// pattern in src/natives/ZymVM.c): the per-VM `ZymCliVmCtx*` is owned
// by a single finalizer-bearing `zym_createNativeContext` value
// produced in `nativeZym_create`. Every `Zym.*` method closure shares
// that context, so the ctx pointer travels with the closures. When
// the VM tears down its globals, the closures drop, the shared
// context is GC'd, and the finalizer below frees the ctx (unless the
// ctx is marked `externalOwner` — see `ZymCliVmCtx::externalOwner`,
// used for child VMs whose ctx is owned by the parent's
// `ChildVmHandle`). There is no process-wide bookkeeping.
//
// PR 2 adds the nested-VM surface: `Zym.newVM()` and the setup-phase
// methods on the returned ChildVM struct. The ChildVM closures live
// in the *parent* VM but operate on the *child* VM. Their context
// is a `ChildVmHandle` (parent-allocated) that holds:
//   - the child `ZymVM*` pointer,
//   - the child's `ZymCliVmCtx*` (externalOwner=true; freed by the
//     handle finalizer, not by the child's Zym finalizer),
//   - the parent's `ZymCliVmCtx*` (borrowed; used for capability
//     resolution in registerCliNative / "ALL"),
//   - a `freed` flag so an explicit `vm.free()` and the finalizer
//     don't both call `zym_freeVM`.

#include "zym_native.hpp"

#include "../cli_catalog.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace {

// =============================================================================
// CTX FINALIZER (shared by parent's Zym module and any child that gets Zym
// installed via `cli_catalog_install_named`)
// =============================================================================

void zym_ctx_finalizer(ZymVM* /*vm*/, void* p) {
    auto* ctx = static_cast<ZymCliVmCtx*>(p);
    if (!ctx) return;
    if (ctx->externalOwner) return;  // owned elsewhere; see ZymCliVmCtx
    delete ctx;
}

// =============================================================================
// ChildVmHandle — parent-side state backing a `ChildVM` script value
// =============================================================================
//
// Allocated by `Zym.newVM()` in the parent VM. Bound as the userdata
// of one finalizer-bearing native context value shared by every
// ChildVM method closure (`registerCliNative`, `defineGlobal`, ...).
// When the parent VM tears down its globals, the ChildVM map drops,
// the shared context is GC'd, the finalizer below fires, and:
//   1. if `freed == false`, calls `zym_freeVM(child)` (which fires
//      any finalizers in the child, including the child-side Zym
//      ctx finalizer if Zym was granted — that one is a no-op because
//      `externalOwner == true`),
//   2. deletes `childCtx`,
//   3. deletes the handle itself.
//
// `parentCtx` is a borrowed pointer into the parent's own ctx; it is
// used only during normal method dispatch (capability resolution),
// never during finalization.

struct ChildVmHandle {
    ZymVM* child = nullptr;
    ZymCliVmCtx* childCtx = nullptr;
    ZymCliVmCtx* parentCtx = nullptr;
    bool freed = false;
};

void child_handle_finalizer(ZymVM* /*parentVm*/, void* p) {
    auto* h = static_cast<ChildVmHandle*>(p);
    if (!h) return;
    if (h->child && !h->freed) {
        zym_freeVM(h->child);
        h->freed = true;
    }
    // childCtx is externalOwner=true, so the child's Zym finalizer
    // (if Zym was granted) skipped freeing it. Free it here.
    delete h->childCtx;
    delete h;
}

// =============================================================================
// Helpers
// =============================================================================

// Returns true if `set` contains `name` (string equality).
bool set_has(const std::vector<std::string>& set, const char* name) {
    if (!name) return false;
    for (const auto& s : set) {
        if (s == name) return true;
    }
    return false;
}

// Capability-and-phase guard for ChildVM methods. Returns nullptr
// (after raising a runtime error) if the call is invalid; otherwise
// returns the handle. `setupOnly` enforces `phase == Setup`.
//
// All "the call cannot proceed" cases collapse to a single
// "no such native" runtime error (roadmap §0 → Errors): the script
// must not be able to tell whether the method was called on a freed
// VM, on a post-freeze VM, or just doesn't exist. From the script's
// perspective, the capability is simply absent.
ChildVmHandle* require_child(ZymVM* vm, ZymValue context, bool setupOnly) {
    auto* h = static_cast<ChildVmHandle*>(zym_getNativeData(context));
    if (!h || h->freed || !h->child || !h->childCtx) {
        zym_runtimeError(vm, "no such native");
        return nullptr;
    }
    if (setupOnly && h->childCtx->phase != ZymVmPhase::Setup) {
        zym_runtimeError(vm, "no such native");
        return nullptr;
    }
    return h;
}

// Resolves a single grant name against the *parent's* capability
// set (never the global catalog) and installs it on the child.
// Returns false (after raising) if the name is unknown or withheld.
// Idempotent: re-granting an already-installed name is a silent
// no-op (cli_catalog_install_named handles this internally).
bool grant_one(ZymVM* parentVm, ChildVmHandle* h, const char* name) {
    if (!name) {
        zym_runtimeError(parentVm, "no such native");
        return false;
    }
    if (!set_has(h->parentCtx->available, name)) {
        // Unknown OR withheld: indistinguishable to the script.
        zym_runtimeError(parentVm, "no such native: %s", name);
        return false;
    }
    cli_catalog_install_named(h->child, h->childCtx, name);
    return true;
}

// =============================================================================
// Zym.* methods (top-level singleton, parent-VM closures)
// =============================================================================

ZymValue z_cliNatives(ZymVM* vm, ZymValue context) {
    auto* cli = static_cast<ZymCliVmCtx*>(zym_getNativeData(context));
    ZymValue list = zym_newList(vm);
    if (!cli) return list;
    for (const auto& name : cli->available) {
        zym_listAppend(vm, list, zym_newString(vm, name.c_str()));
    }
    return list;
}

// forward decl: defined below the ChildVM section so it can see the
// methods table.
ZymValue make_child_vm(ZymVM* parentVm, ChildVmHandle* handle);

ZymValue z_newVM(ZymVM* parentVm, ZymValue context) {
    auto* parentCtx = static_cast<ZymCliVmCtx*>(zym_getNativeData(context));
    if (!parentCtx) {
        zym_runtimeError(parentVm, "no such native");
        return ZYM_ERROR;
    }

    // Allocator is inherited (roadmap §0). Scripts cannot select one.
    const ZymAllocator* alloc = zym_getAllocator(parentVm);
    ZymVM* child = zym_newVM(const_cast<ZymAllocator*>(alloc));

    // Buffer is the only auto-installed native (roadmap §0).
    cli_catalog_install_auto(child);

    auto* childCtx = new ZymCliVmCtx();
    childCtx->vm = child;
    childCtx->externalOwner = true;  // owned by handle below

    auto* handle = new ChildVmHandle();
    handle->child = child;
    handle->childCtx = childCtx;
    handle->parentCtx = parentCtx;

    return make_child_vm(parentVm, handle);
}

// =============================================================================
// ChildVM.* methods (closures live in parent VM, operate on child VM)
// =============================================================================

ZymValue cv_cliNatives(ZymVM* parentVm, ZymValue context) {
    ZymValue list = zym_newList(parentVm);
    auto* h = require_child(parentVm, context, /*setupOnly*/ false);
    if (!h) return list;  // require_child already raised
    for (const auto& n : h->childCtx->available) {
        zym_listAppend(parentVm, list, zym_newString(parentVm, n.c_str()));
    }
    return list;
}

ZymValue cv_registerCliNative(ZymVM* parentVm, ZymValue context, ZymValue arg) {
    auto* h = require_child(parentVm, context, /*setupOnly*/ true);
    if (!h) return ZYM_ERROR;

    if (zym_isString(arg)) {
        const char* s = zym_asCString(arg);
        if (s && std::strcmp(s, "ALL") == 0) {
            // Grant every name the parent itself has. Order matches
            // the parent's `available` (declaration order).
            // Snapshot first because grant_one mutates childCtx, not
            // parentCtx — but defensive copy avoids surprises.
            std::vector<std::string> snapshot = h->parentCtx->available;
            for (const auto& n : snapshot) {
                cli_catalog_install_named(h->child, h->childCtx, n.c_str());
            }
            return zym_newBool(true);
        }
        if (!grant_one(parentVm, h, s)) return ZYM_ERROR;
        return zym_newBool(true);
    }

    if (zym_isList(arg)) {
        int n = zym_listLength(arg);
        for (int i = 0; i < n; i++) {
            ZymValue v = zym_listGet(parentVm, arg, i);
            if (!zym_isString(v)) {
                zym_runtimeError(parentVm,
                    "registerCliNative([...]) expects a list of strings");
                return ZYM_ERROR;
            }
            if (!grant_one(parentVm, h, zym_asCString(v))) return ZYM_ERROR;
        }
        return zym_newBool(true);
    }

    zym_runtimeError(parentVm,
        "registerCliNative(arg) expects a string, list of strings, or \"ALL\"");
    return ZYM_ERROR;
}

ZymValue cv_defineGlobal(ZymVM* parentVm, ZymValue context,
                         ZymValue nameV, ZymValue value) {
    auto* h = require_child(parentVm, context, /*setupOnly*/ true);
    if (!h) return ZYM_ERROR;

    if (!zym_isString(nameV)) {
        zym_runtimeError(parentVm, "defineGlobal(name, value) expects a string name");
        return ZYM_ERROR;
    }
    const char* name = zym_asCString(nameV);

    // PR 2: primitives only — string, number, bool. Lists, maps,
    // structs, Buffer, callables land in PR 4 with the cross-VM
    // marshaller.
    ZymValue marshalled;
    if (zym_isString(value)) {
        marshalled = zym_newString(h->child, zym_asCString(value));
    } else if (zym_isNumber(value)) {
        marshalled = zym_newNumber(zym_asNumber(value));
    } else if (zym_isBool(value)) {
        marshalled = zym_newBool(zym_asBool(value));
    } else {
        zym_runtimeError(parentVm,
            "defineGlobal(name, value): only strings, numbers, and bools "
            "are marshalled across VMs in this version");
        return ZYM_ERROR;
    }

    zym_defineGlobal(h->child, name, marshalled);
    return zym_newBool(true);
}

ZymValue cv_free(ZymVM* /*parentVm*/, ZymValue context) {
    auto* h = static_cast<ChildVmHandle*>(zym_getNativeData(context));
    if (!h) return zym_newBool(false);
    if (h->freed || !h->child) return zym_newBool(false);
    zym_freeVM(h->child);
    h->freed = true;
    h->child = nullptr;
    // Do NOT delete childCtx here — the parent's native context
    // finalizer will do that on parent VM teardown. Subsequent
    // ChildVM method calls hit `require_child` which sees `freed`
    // and raises "no such native".
    return zym_newBool(true);
}

// =============================================================================
// ChildVM struct assembly
// =============================================================================

ZymValue make_child_vm(ZymVM* parentVm, ChildVmHandle* handle) {
    ZymValue ctx = zym_createNativeContext(parentVm, handle, child_handle_finalizer);
    zym_pushRoot(parentVm, ctx);

    ZymValue mRegister = zym_createNativeClosure(
        parentVm, "registerCliNative(arg)", (void*)cv_registerCliNative, ctx);
    zym_pushRoot(parentVm, mRegister);

    ZymValue mDefine = zym_createNativeClosure(
        parentVm, "defineGlobal(name, value)", (void*)cv_defineGlobal, ctx);
    zym_pushRoot(parentVm, mDefine);

    ZymValue mCli = zym_createNativeClosure(
        parentVm, "cliNatives()", (void*)cv_cliNatives, ctx);
    zym_pushRoot(parentVm, mCli);

    ZymValue mFree = zym_createNativeClosure(
        parentVm, "free()", (void*)cv_free, ctx);
    zym_pushRoot(parentVm, mFree);

    ZymValue obj = zym_newMap(parentVm);
    zym_pushRoot(parentVm, obj);

    zym_mapSet(parentVm, obj, "registerCliNative", mRegister);
    zym_mapSet(parentVm, obj, "defineGlobal",      mDefine);
    zym_mapSet(parentVm, obj, "cliNatives",        mCli);
    zym_mapSet(parentVm, obj, "free",              mFree);

    // ctx + 4 closures + obj = 6
    for (int i = 0; i < 6; i++) zym_popRoot(parentVm);
    return obj;
}

} // namespace

ZymValue nativeZym_create(ZymVM* vm, ZymCliVmCtx* ctx) {
    // The shared context wraps `ctx` with the finalizer that owns
    // its lifetime (unless `ctx->externalOwner` says otherwise — see
    // `zym_ctx_finalizer`). Every Zym.* method closure binds to this
    // exact value, so all methods see the same ctx and the ctx is
    // freed exactly once when all closures are gone.
    ZymValue sharedCtx = zym_createNativeContext(vm, ctx, zym_ctx_finalizer);
    zym_pushRoot(vm, sharedCtx);

    ZymValue mCli = zym_createNativeClosure(
        vm, "cliNatives()", (void*)z_cliNatives, sharedCtx);
    zym_pushRoot(vm, mCli);

    ZymValue mNew = zym_createNativeClosure(
        vm, "newVM()", (void*)z_newVM, sharedCtx);
    zym_pushRoot(vm, mNew);

    ZymValue obj = zym_newMap(vm);
    zym_pushRoot(vm, obj);

    zym_mapSet(vm, obj, "cliNatives", mCli);
    zym_mapSet(vm, obj, "newVM",      mNew);

    // sharedCtx + cliNatives + newVM + obj = 4
    for (int i = 0; i < 4; i++) zym_popRoot(vm);
    return obj;
}
