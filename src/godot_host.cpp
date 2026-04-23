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
#include "core/register_core_types.h"

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
    register_core_types();
    g_packed_data      = memnew(PackedData);
    g_project_settings = memnew(ProjectSettings);
    register_core_settings();
    register_early_core_singletons();
    register_core_singletons();

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

    unregister_core_types();

    if (g_os) {
        // finalize_core() lives on OS_Unix.
        static_cast<OS_Unix*>(g_os)->finalize_core();
        delete g_os;
        g_os = nullptr;
    }

    g_initialized = false;
}

} // namespace zym::godot_host
