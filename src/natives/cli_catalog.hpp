#pragma once

// Capability-gated CLI native catalog.
//
// Today every ZymVM that the CLI spins up gets its globals via a single
// monolithic `setupNatives(vm)` pass (see natives.cpp). Going forward
// (see future/zym_roadmap.md) scripts will be able to spin up nested
// in-process VMs and grant them only a subset of the catalog. This
// file is the shared substrate: a name-keyed registry of installer
// functions, plus a per-VM context tracking the lifecycle phase and
// the set of catalog names the VM is allowed to grant onward.
//
// Behaviourally, in PR 1, this changes nothing: `cli_catalog_install_all`
// produces exactly the same globals `setupNatives` did, in the same
// order, and seeds the root VM's `available` set with every grantable
// name. Subsequent PRs add the per-VM phase guard and the `Zym.newVM`
// surface that consumes this catalog.
//
// `Buffer` is the *only* native that is not gated by the catalog: it
// is auto-installed on every VM regardless of grants, because Zym
// scripts treat it as a primitive (see roadmap §0). It does not appear
// in `cli_catalog_names()` and cannot be granted via
// `cli_catalog_install_named`; it lands via `cli_catalog_install_auto`.
//
// `Zym` itself is a regular catalog entry. The root has it (because
// the root has the full catalog), and parents grant it explicitly to
// children that should be allowed to spawn nested VMs.

#include "zym/zym.h"

#include <string>
#include <vector>

enum class ZymVmPhase {
    Setup,      // registerCliNative / registerNative / defineGlobal allowed
    Execution,  // frozen: only pipeline / call APIs allowed
};

// Per-VM CLI state. Ownership of the heap allocation transfers to
// the `Zym` native module's closure context: when the catalog
// installer hands the ctx to `nativeZym_create`, that native binds
// the pointer into a `zym_createNativeContext` with a finalizer.
// Every `Zym.*` method closure shares that one context, so the ctx
// travels with the closures (pre-godot CLI's `VMData` pattern in
// `src/natives/ZymVM.c`). VM teardown drops the closures, fires the
// finalizer, and frees the ctx — no process-wide bookkeeping.
struct ZymCliVmCtx {
    ZymVM* vm = nullptr;
    ZymVmPhase phase = ZymVmPhase::Setup;
    // Declaration-order list of catalog names this VM is allowed to
    // grant to children. `vector<string>` instead of `unordered_set`
    // so iteration / `cliNatives()` returns a stable order without
    // an extra sort.
    std::vector<std::string> available;
};

// Returns the declaration-order list of grantable catalog names (does
// NOT include `Buffer`, which is auto-installed). Stable across calls.
const std::vector<std::string>& cli_catalog_names();

// True if `name` is a grantable catalog entry. False for `Buffer`,
// for unknown names, and for `nullptr`.
bool cli_catalog_has(const char* name);

// Install the auto-natives (`Buffer` only) on `vm`. Always-on; not
// affected by capabilities. Safe to call multiple times.
void cli_catalog_install_auto(ZymVM* vm);

// Install one catalog entry by name on `vm`, recording it in `ctx`'s
// `available` list. Returns false if `name` is not a grantable
// catalog entry (Buffer is rejected here; use
// `cli_catalog_install_auto`).
//
// Idempotent: re-granting an already-installed name is a silent
// no-op (matches the user-locked policy — scripts can call
// `registerCliNative("ALL")` regardless of prior state). Collisions
// with a script-defined global are last-write-wins, mirroring the
// underlying `zym_defineGlobal` semantics.
//
// Note: callers must own `ctx` and pass it explicitly. The catalog
// no longer maintains a process-wide VM→ctx map; the Zym native
// module is the canonical owner.
bool cli_catalog_install_named(ZymVM* vm, ZymCliVmCtx* ctx, const char* name);

// Install everything: auto-natives + every catalog entry, allocates
// a fresh `ZymCliVmCtx`, populates its `available` list with the
// full catalog, and installs `Zym` last (handing ctx ownership to
// the `Zym` native's closure context, which holds the finalizer
// that frees the allocation on VM teardown). Used for the root VM
// at boot and as the byte-for-byte replacement for the legacy
// `setupNatives`.
void cli_catalog_install_all(ZymVM* vm);
