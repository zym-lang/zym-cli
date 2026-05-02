#include "cli_catalog.hpp"

#include "natives.hpp"
#include "Zym/zym_native.hpp"

#include <array>
#include <cstring>

// =============================================================================
// CATALOG TABLE
// =============================================================================
//
// Each grantable entry pairs a script-visible name with an `installer`
// that performs the same `zym_defineGlobal(...)` (or
// `zym_defineNativeVariadic`) the legacy `setupNatives` performed for
// that name. Order matches the original `setupNatives` body
// (src/natives/natives.cpp before this PR), so calling
// `cli_catalog_install_all(vm)` produces a byte-for-byte equivalent
// VM globals layout.
//
// `Buffer` lives in the auto-install list, not here. `Zym` is added
// at the end of the grantable list as the new entry that gates
// nested-VM creation.

namespace {

using InstallerFn = void (*)(ZymVM*);

struct CatalogEntry {
    const char* name;
    InstallerFn install;
};

// ----- installers ---------------------------------------------------------

void install_print  (ZymVM* vm) { zym_defineNativeVariadic(vm, "print(...)", (void*)nativePrint); }
void install_time   (ZymVM* vm) { zym_defineGlobal(vm, "Time",    nativeTime_create(vm));    }
void install_file   (ZymVM* vm) { zym_defineGlobal(vm, "File",    nativeFile_create(vm));    }
void install_dir    (ZymVM* vm) { zym_defineGlobal(vm, "Dir",     nativeDir_create(vm));     }
void install_console(ZymVM* vm) { zym_defineGlobal(vm, "Console", nativeConsole_create(vm)); }
void install_process(ZymVM* vm) { zym_defineGlobal(vm, "Process", nativeProcess_create(vm)); }
void install_regex  (ZymVM* vm) { zym_defineGlobal(vm, "RegEx",   nativeRegex_create(vm));   }
void install_json   (ZymVM* vm) { zym_defineGlobal(vm, "JSON",    nativeJson_create(vm));    }
void install_crypto (ZymVM* vm) { zym_defineGlobal(vm, "Crypto",  nativeCrypto_create(vm));  }
void install_random (ZymVM* vm) { zym_defineGlobal(vm, "Random",  nativeRandom_create(vm)); }
void install_hash   (ZymVM* vm) { zym_defineGlobal(vm, "Hash",    nativeHash_create(vm));    }
void install_system (ZymVM* vm) { zym_defineGlobal(vm, "System",  nativeSystem_create(vm));  }
void install_path   (ZymVM* vm) { zym_defineGlobal(vm, "Path",    nativePath_create(vm));    }
void install_ip     (ZymVM* vm) { zym_defineGlobal(vm, "IP",      nativeIp_create(vm));      }
void install_tcp    (ZymVM* vm) { zym_defineGlobal(vm, "TCP",     nativeTcp_create(vm));     }
void install_udp    (ZymVM* vm) { zym_defineGlobal(vm, "UDP",     nativeUdp_create(vm));     }
void install_tls    (ZymVM* vm) { zym_defineGlobal(vm, "TLS",     nativeTls_create(vm));     }
void install_dtls   (ZymVM* vm) { zym_defineGlobal(vm, "DTLS",    nativeDtls_create(vm));    }
void install_enet   (ZymVM* vm) { zym_defineGlobal(vm, "ENet",    nativeEnet_create(vm));    }
void install_aes    (ZymVM* vm) { zym_defineGlobal(vm, "AES",     nativeAes_create(vm));     }
void install_sockets(ZymVM* vm) { zym_defineGlobal(vm, "Sockets", nativeSockets_create(vm)); }
// Note: `install_zym` is intentionally absent from the kCatalog
// installer slot. Zym is installed by `cli_catalog_install_all` /
// `cli_catalog_install_named` *after* the rest of the catalog so it
// receives the fully-populated `ZymCliVmCtx*` and can take ownership
// of it via a finalizer-bearing closure context. See
// `nativeZym_create` for the lifetime contract.

// Order matches the legacy `setupNatives` body, with `Zym` appended
// as the new grantable entry. `Buffer` is intentionally absent
// (auto-installed). When a new module is added, append it to this
// table and to the corresponding declaration in natives.hpp.
constexpr std::array<CatalogEntry, 21> kCatalog = {{
    {"print",   install_print},
    {"Time",    install_time},
    {"File",    install_file},
    {"Dir",     install_dir},
    {"Console", install_console},
    {"Process", install_process},
    {"RegEx",   install_regex},
    {"JSON",    install_json},
    {"Crypto",  install_crypto},
    {"Random",  install_random},
    {"Hash",    install_hash},
    {"System",  install_system},
    {"Path",    install_path},
    {"IP",      install_ip},
    {"TCP",     install_tcp},
    {"UDP",     install_udp},
    {"TLS",     install_tls},
    {"DTLS",    install_dtls},
    {"ENet",    install_enet},
    {"AES",     install_aes},
    {"Sockets", install_sockets},
}};

const CatalogEntry* find_entry(const char* name) {
    if (!name) return nullptr;
    for (const auto& e : kCatalog) {
        if (std::strcmp(e.name, name) == 0) return &e;
    }
    return nullptr;
}

// ----- per-VM ctx lifetime ------------------------------------------------
//
// `ZymCliVmCtx` is heap-allocated by `cli_catalog_install_all` (root
// VM) or by `Zym.newVM` (PR 2 onward) and handed off to
// `nativeZym_create`, which binds it as the userdata of a
// finalizer-bearing native context shared by every `Zym.*` method
// closure. The finalizer below frees the allocation when the closure
// context is GC'd (i.e. when the VM is torn down and its globals
// drop). No process-wide bookkeeping; the ctx travels with the
// closures, mirroring the `VMData`/`zymvm_cleanup` pattern in the
// pre-godot CLI's `src/natives/ZymVM.c`.

bool ctx_has(const ZymCliVmCtx* ctx, const char* name) {
    for (const auto& s : ctx->available) {
        if (s == name) return true;
    }
    return false;
}

} // namespace

// =============================================================================
// PUBLIC API
// =============================================================================

const std::vector<std::string>& cli_catalog_names() {
    static const std::vector<std::string> names = []() {
        std::vector<std::string> v;
        v.reserve(kCatalog.size());
        for (const auto& e : kCatalog) v.emplace_back(e.name);
        return v;
    }();
    return names;
}

bool cli_catalog_has(const char* name) {
    return find_entry(name) != nullptr;
}

void cli_catalog_install_auto(ZymVM* vm) {
    // `Buffer` is the only auto-installed native (see roadmap §0).
    // Stays out of the grantable catalog and out of `cliNatives()`.
    zym_defineGlobal(vm, "Buffer", nativeBuffer_create(vm));
}

bool cli_catalog_install_named(ZymVM* vm, ZymCliVmCtx* ctx, const char* name) {
    if (!ctx) return false;
    if (!name) return false;

    // The `Zym` entry is special: it isn't an installer in `kCatalog`
    // (it can't be, because the installer signature has no place for
    // the ctx pointer it needs to take ownership of). Handle it here.
    if (std::strcmp(name, "Zym") == 0) {
        if (ctx_has(ctx, "Zym")) return true;  // idempotent
        ctx->available.emplace_back("Zym");
        zym_defineGlobal(vm, "Zym", nativeZym_create(vm, ctx));
        return true;
    }

    const CatalogEntry* e = find_entry(name);
    if (!e) return false;

    if (ctx_has(ctx, e->name)) {
        // Idempotent: already granted, treat as success without
        // reinstalling the global. Matches the user-locked policy:
        // scripts can call `registerCliNative("ALL")` regardless of
        // prior state; only un-granted names get installed.
        return true;
    }

    e->install(vm);
    ctx->available.emplace_back(e->name);
    return true;
}

void cli_catalog_install_all(ZymVM* vm) {
    cli_catalog_install_auto(vm);

    // Fresh ctx; ownership transfers to `nativeZym_create` below.
    auto* ctx = new ZymCliVmCtx();
    ctx->vm = vm;

    // Install every grantable catalog entry except Zym, which is
    // installed last so it can adopt the now-populated ctx.
    for (const auto& e : kCatalog) {
        e.install(vm);
        ctx->available.emplace_back(e.name);
    }

    // Install Zym last; this transfers ownership of `ctx` to the
    // Zym native's closure context (finalizer attached there).
    ctx->available.emplace_back("Zym");
    zym_defineGlobal(vm, "Zym", nativeZym_create(vm, ctx));
}
