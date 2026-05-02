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

// Per-VM CLI state. One is auto-attached the first time any
// cli_catalog_* function touches a VM, and auto-freed when the VM is
// torn down (via a finalizer-bearing native context that we
// permanently root in the VM; the VM teardown drops the root, the
// finalizer fires, and the entry is removed from the lookup map).
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

// Install one catalog entry by name and add it to the VM's
// `available` set. Returns false if `name` is not a grantable entry
// (Buffer is rejected here; use `cli_catalog_install_auto`).
// Idempotent: re-installing an already-granted name is a no-op.
bool cli_catalog_install_named(ZymVM* vm, const char* name);

// Install everything: auto-natives + every catalog entry, seeding
// `available` with the full catalog. Used for the root VM at boot
// and as the byte-for-byte replacement for the legacy `setupNatives`.
void cli_catalog_install_all(ZymVM* vm);

// Look up the per-VM ctx, attaching one if not already present.
// Never returns nullptr (allocation failure aborts via the engine
// like other natives do).
ZymCliVmCtx* cli_catalog_ctx(ZymVM* vm);
