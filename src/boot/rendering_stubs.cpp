// =============================================================================
// src/boot/rendering_stubs.cpp
// -----------------------------------------------------------------------------
// Link-time stubs that satisfy the three engine symbols pulled in by
// `Engine::set_max_fps()` (godot/core/config/engine.cpp:86) and
// `OS_LinuxBSD::get_video_adapter_driver_info()`
// (godot/platform/linuxbsd/os_linuxbsd.cpp:332):
//
//   _ZN15RenderingDevice13get_singletonEv   -- RenderingDevice::get_singleton()
//   _ZN15RenderingDevice13_set_max_fpsEi    -- RenderingDevice::_set_max_fps(int)
//   _ZN15RenderingServer13get_singletonEv   -- RenderingServer::get_singleton()
//
// Why this file exists
// --------------------
// Both call sites are vtable-pinned by zym's own boot path:
//   - `memnew(Engine)` in src/godot_host.cpp pins `Engine::set_max_fps`
//     (declared `virtual` in core/config/engine.h:128), whose body calls
//     `RenderingDevice::get_singleton()` and `_set_max_fps`.
//   - `memnew(OS_LinuxBSD)` in src/boot/register_core.cpp pins
//     `OS_LinuxBSD::get_video_adapter_driver_info()` (override of an
//     `OS::` virtual), whose body calls `RenderingServer::get_singleton()`.
//
// Both bodies are compiled into libcore.a / libplatform.a by godot's SCons
// build with the *real* rendering_device.h / rendering_server.h in scope,
// so the LTO IR carries hard external references to the mangled names
// above. A header shim alone cannot rewrite those references after the
// fact (`#include_next`-style shimming -- the technique used for ClassDB
// in scripts/zym_shim/core/object/class_db.h -- only works for
// preprocessor-macro call sites, not direct C++ method calls). Replacing
// the headers wholesale would also break the unrelated TUs (the real
// `rendering_device.cpp` / `rendering_server.cpp`) that godot's SCons
// still globs in via `servers/rendering/SCsub`'s `*.cpp` rule, since
// there is no `RENDERING_DEVICE_DISABLED` / `RENDERING_SERVER_DISABLED`
// gate anywhere in the engine source.
//
// The cheapest no-godot-edits resolution is to provide the missing
// symbols at final-link time. The `if (rd)` and
// `if (RenderingServer::get_singleton() == nullptr)` guards already
// present in those engine bodies make the runtime behaviour correct
// when the stubs return nullptr -- both functions early-return without
// touching anything renderer-related, which is the desired CLI
// behaviour anyway.
//
// ODR / layout notes
// ------------------
// We deliberately do NOT include godot's real `rendering_device.h` or
// `rendering_server.h` here -- they drag in 100+ engine headers and
// would defeat the point. Instead we forward-declare minimal classes
// with just the three member signatures we need to define. Under the
// Itanium C++ ABI (Linux/x86_64), the mangled name of a non-virtual
// static / member function depends only on the qualified name and
// parameter types, not on the rest of the class layout, so the
// definitions emitted from this TU bind to the same external
// references the engine TUs emit.
//
// There is no ODR conflict at link time: zym's `libservers.a` is
// excluded from the link line precisely because we're stubbing these
// symbols, so the *real* `RenderingDevice::get_singleton` /
// `RenderingServer::get_singleton` / `RenderingDevice::_set_max_fps`
// definitions never reach the linker -- only ours do.
//
// Unlocks: dropping `libservers.a` (and transitively
// `ShaderLanguage::keyword_list`, the audio/physics/nav/XR server TUs,
// and their `__static_initialization_and_destruction_0()`
// contributions) from `ZYM_GODOT_SUBLIBS` in the top-level
// CMakeLists.txt. See linker errors in the issue history for the exact
// undefined-reference set this stub resolves.
// =============================================================================

class RenderingDevice {
public:
    static RenderingDevice *get_singleton();
    void _set_max_fps(int p_max_fps);
};

class RenderingServer {
public:
    static RenderingServer *get_singleton();
};

RenderingDevice *RenderingDevice::get_singleton() {
    return nullptr;
}

void RenderingDevice::_set_max_fps(int /*p_max_fps*/) {
    // No-op: there is no rendering device in the zym CLI build, so any
    // attempt to push an FPS cap to a swapchain is meaningless. The
    // Engine::set_max_fps caller already null-checks the singleton
    // before invoking this, so this body is unreachable in practice;
    // it exists purely to provide a definition the linker can resolve
    // against.
}

RenderingServer *RenderingServer::get_singleton() {
    return nullptr;
}
