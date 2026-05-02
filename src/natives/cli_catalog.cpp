#include "cli_catalog.hpp"

#include "natives.hpp"
#include "Zym/zym_native.hpp"

#include <array>
#include <cstring>
#include <unordered_map>
#include <utility>

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
void install_zym    (ZymVM* vm) { zym_defineGlobal(vm, "Zym",     nativeZym_create(vm));     }

// Order matches the legacy `setupNatives` body, with `Zym` appended
// as the new grantable entry. `Buffer` is intentionally absent
// (auto-installed). When a new module is added, append it to this
// table and to the corresponding declaration in natives.hpp.
constexpr std::array<CatalogEntry, 22> kCatalog = {{
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
    {"Zym",     install_zym},
}};

const CatalogEntry* find_entry(const char* name) {
    if (!name) return nullptr;
    for (const auto& e : kCatalog) {
        if (std::strcmp(e.name, name) == 0) return &e;
    }
    return nullptr;
}

// ----- per-VM ctx storage -------------------------------------------------
//
// VM addresses are unique within the process and stable for the VM's
// lifetime, so they make a fine map key. Auto-cleanup is wired through
// a finalizer-bearing native context that we permanently root in the
// VM (`zym_pushRoot` without a balancing pop): when the VM is torn
// down, root drops trigger the finalizer, which removes the map
// entry. We never expose this holder value to script.

std::unordered_map<ZymVM*, ZymCliVmCtx*>& ctx_map() {
    static std::unordered_map<ZymVM*, ZymCliVmCtx*> m;
    return m;
}

void ctx_finalizer(ZymVM* /*vm*/, void* p) {
    auto* ctx = static_cast<ZymCliVmCtx*>(p);
    if (!ctx) return;
    ctx_map().erase(ctx->vm);
    delete ctx;
}

ZymCliVmCtx* attach_ctx(ZymVM* vm) {
    auto* ctx = new ZymCliVmCtx();
    ctx->vm = vm;
    ctx_map()[vm] = ctx;

    // Permanently-rooted finalizer holder. Never popped on purpose:
    // the root keeps the holder alive for the VM's life, and VM
    // teardown drops the root, firing `ctx_finalizer`.
    ZymValue holder = zym_createNativeContext(vm, ctx, ctx_finalizer);
    zym_pushRoot(vm, holder);
    return ctx;
}

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

bool cli_catalog_install_named(ZymVM* vm, const char* name) {
    const CatalogEntry* e = find_entry(name);
    if (!e) return false;

    ZymCliVmCtx* ctx = cli_catalog_ctx(vm);
    if (ctx_has(ctx, e->name)) {
        // Idempotent: already granted, treat as success without
        // reinstalling the global (the legacy installers are not
        // generally safe to call twice, and there's no script-visible
        // difference between a fresh install and a re-install).
        return true;
    }

    e->install(vm);
    ctx->available.emplace_back(e->name);
    return true;
}

void cli_catalog_install_all(ZymVM* vm) {
    cli_catalog_install_auto(vm);
    ZymCliVmCtx* ctx = cli_catalog_ctx(vm);
    for (const auto& e : kCatalog) {
        if (ctx_has(ctx, e.name)) continue;
        e.install(vm);
        ctx->available.emplace_back(e.name);
    }
}

ZymCliVmCtx* cli_catalog_ctx(ZymVM* vm) {
    auto& m = ctx_map();
    auto it = m.find(vm);
    if (it != m.end()) return it->second;
    return attach_ctx(vm);
}
