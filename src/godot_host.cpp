// Headless subset of Godot's Main::test_setup() / test_cleanup().
// Skips servers, scene, modules, extensions, editor - core IO + print only.

#include "godot_host.hpp"

// Widen OS::initialize/finalize/finalize_core (protected, friend=Main) to
// public, confined to this TU. Subclassing triggers an RTTI chain that
// libgodot.a (built -fno-rtti) can't satisfy; modifying Godot headers is
// off-limits; Main::setup() pulls in stripped subsystems.
#define protected public
#include "drivers/unix/os_unix.h"
#include "platform/linuxbsd/os_linuxbsd.h"
#undef protected

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
OS_LinuxBSD*     g_os               = nullptr;
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
    g_os = new OS_LinuxBSD();
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
    g_project_settings = memnew(ProjectSettings);
    // register_core_settings() intentionally NOT called: its 6 GLOBAL_DEFs
    // configure (a) network/limits/{tcp,unix,packet_peer_stream} +
    // network/tls/certificate_bundle_override -- consumed only by the
    // network/TLS class graph already permanently skipped via the 8
    // register_custom_instance_class<T>() deletions (HTTPClient, Crypto,
    // StreamPeerTLS, PacketPeerDTLS, ...), and (b) threading/worker_pool/*
    // -- consumed only by WorkerThreadPool::init(), which zym never reaches
    // because zym::boot::register_core_types() does not memnew the
    // WorkerThreadPool singleton (engine register_core_types.cpp:341 was
    // its only construction site). Nothing in zym's link graph queries
    // any of these keys, so the GLOBAL_DEFs would be orphaned defaults.
    // Skipping the call lets --gc-sections evict register_core_settings()
    // itself plus the 4 PropertyInfo ctors it constructs.
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
        // finalize_core() lives on OS_Unix.
        static_cast<OS_Unix*>(g_os)->finalize_core();
        delete g_os;
        g_os = nullptr;
    }

    g_initialized = false;
}

} // namespace zym::godot_host
