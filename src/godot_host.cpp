// Headless subset of Godot's Main::test_setup() / test_cleanup().
// Skips servers, scene, modules, extensions, editor - core IO + print only.

#include "godot_host.hpp"

// Widen OS::initialize/finalize/finalize_core (protected, friend=Main) to
// public, confined to this TU. Subclassing triggers an RTTI chain that
// libgodot.a (built -fno-rtti) can't satisfy; modifying Godot headers is
// off-limits; Main::setup() pulls in stripped subsystems.
#define protected public
#ifdef _WIN32
#include "platform/windows/os_windows.h"
#else
#include "drivers/unix/os_unix.h"
#include "platform/linuxbsd/os_linuxbsd.h"
#endif
#undef protected

#ifdef _WIN32
// Match the platform OS class so the OS_Unix `finalize_core()` cast below
// can be selected per-platform too.
using ZymPlatformOS = OS_Windows;
#else
using ZymPlatformOS = OS_LinuxBSD;
#endif

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/core_globals.h"
#include "core/io/file_access_pack.h"
#include "core/os/thread_safe.h"

// zym's hand-rolled, additive replacement for ::register_core_types() /
// ::unregister_core_types(). Bodies start empty; lines are added back only
// when a real test failure proves the engine init is needed. See
// src/boot/register_core.cpp for the strategy and confirmed-skip list.
//
// `core/register_core_types.h` is intentionally NOT included: every engine
// entrypoint it declares (`register_core_types`, `unregister_core_types`,
// `register_core_settings`, `register_early_core_singletons`,
// `register_core_singletons`, `register_core_extensions`,
// `unregister_core_extensions`, `register_core_driver_types`,
// `unregister_core_driver_types`) is either replaced by a zym::boot::*
// hand-roll or permanently skipped (see comments in init() below). With no
// remaining call sites from this TU, `--gc-sections` + LTO can finally
// evict `godot/core/register_core_types.cpp.o` -- and transitively the
// CoreBind::* file-local singletons, the 6 GLOBAL_DEFs, the 8
// register_custom_instance_class<T>() chains, and the
// register_global_constants() body -- from the final zym binary.
#include "boot/register_core.hpp"

namespace zym::godot_host {

namespace {

// Owned singletons. OS base ctor installs itself as OS::get_singleton().
ZymPlatformOS*   g_os               = nullptr;
Engine*          g_engine           = nullptr;
PackedData*      g_packed_data      = nullptr;
ProjectSettings* g_project_settings = nullptr;

bool g_initialized = false;

} // namespace

bool init() {
    if (g_initialized) {
        return true;
    }

    set_current_thread_safe_for_nodes(true);
#ifdef _WIN32
    // OS_Windows requires the process HINSTANCE. There's no WinMain in a CLI
    // build, so fetch it from the loaded image directly.
    g_os = new ZymPlatformOS(GetModuleHandleW(nullptr));
#else
    g_os = new ZymPlatformOS();
#endif
    g_os->initialize();
    CoreGlobals::print_ready = true;
    g_engine = memnew(Engine);
    // Order matches Main::setup(): register_core_types() runs first
    // (it brings up ObjectDB / StringName / CoreStringNames -- foundational
    // state that ProjectSettings's ctor depends on via Object::set on
    // StringName-keyed properties), then PackedData (consulted by
    // ProjectSettings during its ctor for pack-mounted overrides), then
    // ProjectSettings itself.
    zym::boot::register_core_types();
    g_packed_data      = memnew(PackedData);
    // ProjectSettings is required even though zym does not load a project.
    // `StreamPeerTCP::connect_to_host` (and other networking paths)
    // unconditionally dereference `ProjectSettings::get_singleton()` via
    // `GLOBAL_GET("network/limits/tcp/connect_timeout_seconds")`. Leaving
    // the singleton null causes a SIGSEGV on the very first `TCP.connect`.
    g_project_settings = memnew(ProjectSettings);
    // The stock engine's `register_core_settings()` is NOT called as-is
    // (its 6 GLOBAL_DEFs configure network/limits/{unix,packet_peer_stream}
    // + network/tls/certificate_bundle_override -- consumed only by the
    // network/TLS class graph already permanently skipped via the 8
    // register_custom_instance_class<T>() deletions, and
    // threading/worker_pool/* -- consumed only by WorkerThreadPool::init(),
    // which zym never reaches because zym::boot::register_core_types() does
    // not memnew the WorkerThreadPool singleton). Skipping the stock call
    // lets --gc-sections evict the unused GLOBAL_DEFs and their
    // PropertyInfo ctors.
    //
    // HOWEVER: two of those keys are actually live in zym's link graph and
    // their absence causes silent runtime failures:
    //   - network/limits/tcp/connect_timeout_seconds -> read by
    //     `StreamPeerTCP::connect_to_host` at stream_peer_tcp.cpp:78.
    //     Missing -> 0s deadline -> every non-LAN TCP connect aborts on
    //     the first poll() iteration. Symptom: TCP/TLS.connect silently
    //     returns null for any remote that isn't sub-ms-RTT.
    //   - network/tls/enable_tls_v1.3 -> read by
    //     `TLSContextMbedTLS::init_client` at tls_context_mbedtls.cpp:218.
    //     Missing -> cap to TLS 1.2.
    // Both are registered by `zym::boot::register_core_settings()` below,
    // which MUST run after `memnew(ProjectSettings)` because GLOBAL_DEF
    // dereferences `ProjectSettings::get_singleton()` (this exact ordering
    // trap previously turned a one-line patch into a SIGSEGV when the
    // GLOBAL_DEF was placed inside `register_core_types()` instead).
    zym::boot::register_core_settings();
    //
    // register_early_core_singletons() / register_core_singletons() are
    // intentionally NOT called: they wire CoreBind::Engine / OS / OS_Time /
    // Marshalls / EngineDebugger / Geometry2D|3D / ResourceLoader|Saver /
    // ClassDB / IP / TranslationServer / Input / InputMap / GDExtensionManager
    // / ResourceUID / WorkerThreadPool singletons into Engine's name table
    // for `Engine::get_singleton_object("Foo")` reflection. zym never does
    // that lookup -- natives talk to godot via direct C++ symbols
    // (`OS::get_singleton()`, `Time::get_singleton()`, ...), which are
    // independent of the Engine name table.
    //
    // Skipping these calls also avoids ~5.7 KB of teardown leaks that
    // ::unregister_core_types() can't clean up without a matching
    // ::register_core_types() call (the CoreBind::* singletons live as
    // file-local statics in godot/core/register_core_types.cpp).

    g_initialized = true;
    return true;
}

void shutdown() {
    if (!g_initialized) {
        return;
    }

    // Silence prints first so teardown error macros don't re-enter state.
    CoreGlobals::print_ready = false;

    if (g_os) {
        g_os->finalize();
    }

    if (g_packed_data) {
        memdelete(g_packed_data);
        g_packed_data = nullptr;
    }
    if (g_project_settings) {
        memdelete(g_project_settings);
        g_project_settings = nullptr;
    }
    if (g_engine) {
        memdelete(g_engine);
        g_engine = nullptr;
    }

    zym::boot::unregister_core_types();

    if (g_os) {
#ifdef _WIN32
        // OS_Windows inherits finalize_core() directly (no OS_Unix layer).
        g_os->finalize_core();
#else
        // finalize_core() lives on OS_Unix.
        static_cast<OS_Unix*>(g_os)->finalize_core();
#endif
        delete g_os;
        g_os = nullptr;
    }

    g_initialized = false;
}

} // namespace zym::godot_host
