set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Tiny")

if(WIN32)
    # MinGW-w64 GCC doesn't ship the asan/ubsan runtimes, so Debug drops the
    # sanitizer flags on Windows. Everything else (-g -O0, -fno-exceptions,
    # frame pointers) stays the same.
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fno-omit-frame-pointer -fno-exceptions")
    set(CMAKE_C_FLAGS_DEBUG   "-g -O0 -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "")
else()
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer -fno-exceptions")
    set(CMAKE_C_FLAGS_DEBUG   "-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-fsanitize=address,undefined")
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -fno-exceptions -fvisibility=hidden -fvisibility-inlines-hidden -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables")
set(CMAKE_C_FLAGS_RELEASE   "-O3 -DNDEBUG -fvisibility=hidden -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-static-libstdc++ -static-libgcc -Wl,--gc-sections -Wl,-s")
#set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-static-libstdc++ -static-libgcc -Wl,--gc-sections")

set(CMAKE_CXX_FLAGS_TINY "-Oz -DNDEBUG -ffunction-sections -fdata-sections -fno-exceptions -fvisibility=hidden -fno-asynchronous-unwind-tables -fno-unwind-tables")
set(CMAKE_C_FLAGS_TINY   "-Oz -DNDEBUG -ffunction-sections -fdata-sections -fno-exceptions -fvisibility=hidden -fno-asynchronous-unwind-tables -fno-unwind-tables")
set(CMAKE_EXE_LINKER_FLAGS_TINY "-static-libstdc++ -static-libgcc -Wl,--gc-sections -Wl,-s -Wl,-Map,zym_cli.map")

set(PGO_MODE "OFF")
if(PGO_MODE STREQUAL "GEN")
    message(STATUS ">> PGO: GENERATION MODE - Running this build will create profile data.")
    set(CMAKE_CXX_FLAGS        "${CMAKE_CXX_FLAGS} -fprofile-generate")
    set(CMAKE_C_FLAGS          "${CMAKE_C_FLAGS} -fprofile-generate")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fprofile-generate")
elseif(PGO_MODE STREQUAL "USE")
    message(STATUS ">> PGO: USE MODE - Optimizing binary using generated data.")
    set(CMAKE_CXX_FLAGS        "${CMAKE_CXX_FLAGS} -fprofile-use -fprofile-correction")
    set(CMAKE_C_FLAGS          "${CMAKE_C_FLAGS} -fprofile-use -fprofile-correction")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fprofile-use -fprofile-correction")
endif()

set(CMAKE_BUILD_TYPE Debug)
set(USE_COMPRESSION OFF)
# RUNTIME_ONLY: ON builds a runtime-only binary that skips compiling and
# linking `full_executor` and only takes the `runtime_loader` path in main.
set(RUNTIME_ONLY OFF)
# USE_ENGINE_ERRORS: ON keeps Godot's `ERROR: ... at: <fn> (file.cpp:N)` lines
# on stderr (the engine's `ERR_FAIL_*` / `ERR_PRINT` macros). OFF compiles the
# binary so engine-level error and warning output is suppressed at startup
# (zym's own runtime errors are unaffected). See src/boot/register_core.cpp.
set(USE_ENGINE_ERRORS OFF)

# ZYM_SDL: ON builds the `sdl` native (SDL3, vendored under third_party/sdl3)
# and links it into zym. Slice 1 (future/gui.md) — window + event pump only.
# Default depends on RUNTIME_ONLY:
#   * CLI build (RUNTIME_ONLY=OFF) -> ON (UI is a first-class CLI feature).
#   * Runtime-export build (RUNTIME_ONLY=ON) -> OFF (size-sensitive artifact;
#     authors can opt back in to produce a UI-capable runtime export).
if(NOT DEFINED ZYM_SDL)
    if(RUNTIME_ONLY)
        set(ZYM_SDL ON)
    else()
        set(ZYM_SDL ON)
    endif()
endif()

# ZYM_UI: ON builds the `ui` native (Dear ImGui, vendored under
# third_party/imgui) and links it into zym. Slice 1 PR 2 — ImGui
# widgets rendered into an SDL3 window via imgui_impl_sdl3 +
# imgui_impl_sdlrenderer3.
# Soft-depends on ZYM_SDL: if ZYM_UI=ON but ZYM_SDL=OFF, we force
# ZYM_UI to OFF and emit a warning rather than producing a broken
# build. Same default policy as ZYM_SDL: ON for the CLI, OFF for
# runtime-export builds where size dominates.
if(NOT DEFINED ZYM_UI)
    if(RUNTIME_ONLY)
        set(ZYM_UI ON)
    else()
        set(ZYM_UI ON)
    endif()
endif()
if(ZYM_UI AND NOT ZYM_SDL)
    message(WARNING ">> ZYM_UI requires ZYM_SDL — disabling ZYM_UI")
    set(ZYM_UI OFF)
endif()

cmake_minimum_required(VERSION 4.0)
project(zym_cli C CXX)
include(CheckIPOSupported)
check_ipo_supported(RESULT ZYM_LTO_OK OUTPUT ZYM_LTO_ERR)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# add_compile_definitions(DEBUG_CALL)

add_subdirectory(zym_core)

set(ZYM_FRAMES_MAX 4096 CACHE STRING "Max VM call frames (overrides FRAMES_MAX in zym_core/src/vm.h)")
target_compile_definitions(zym_core PUBLIC FRAMES_MAX=${ZYM_FRAMES_MAX})

set(ZYM_STACK_MAX 262144 CACHE STRING "Max VM value stack size (overrides STACK_MAX in zym_core/src/vm.h)")
target_compile_definitions(zym_core PUBLIC STACK_MAX=${ZYM_STACK_MAX})

set(APP_SOURCES
        src/main.cpp
        src/runtime_loader.hpp
        src/runtime_loader.cpp
        src/godot_host.hpp
        src/godot_host.cpp
        src/boot/register_core.hpp
        src/boot/register_core.cpp
        # Link-time stubs for `RenderingDevice::get_singleton`,
        # `RenderingDevice::_set_max_fps(int)`, and
        # `RenderingServer::get_singleton` -- vtable-pinned by
        # `memnew(Engine)` (godot_host.cpp) and `memnew(OS_LinuxBSD)`
        # (register_core.cpp). Defining these here lets the link drop
        # `libservers.a` (commented out in `ZYM_GODOT_SUBLIBS` below) by
        # resolving the three otherwise-undefined references LTO emits
        # from `Engine::set_max_fps` and
        # `OS_LinuxBSD::get_video_adapter_driver_info`. See the file
        # header for full rationale.
        src/boot/rendering_stubs.cpp
        src/pack/zpk_format.h
        src/pack/zpk_writer.hpp
        src/pack/zpk_writer.cpp
        src/pack/zpk_reader.hpp
        src/pack/zpk_reader.cpp
)

if(NOT RUNTIME_ONLY)
    list(APPEND APP_SOURCES
            src/full_executor.hpp
            src/full_executor.cpp
    )
endif()

set(ZYM_NATIVE_SOURCES
        src/natives/natives.hpp
        src/natives/natives.cpp
        src/natives/root_scope.hpp
        src/natives/cli_catalog.hpp
        src/natives/cli_catalog.cpp
        src/natives/Zym/zym_native.hpp
        src/natives/Zym/zym_native.cpp
        src/bridge/cross_vm.hpp
        src/bridge/cross_vm.cpp
        src/natives/print.cpp
        src/natives/time.cpp
        src/natives/buffer.cpp
        src/natives/file.cpp
        src/natives/dir.cpp
        src/natives/console.cpp
        src/natives/process.cpp
        src/natives/regex.cpp
        src/natives/json.cpp
        src/natives/crypto.cpp
        src/natives/rng.cpp
        src/natives/hash.cpp
        src/natives/system.cpp
        src/natives/path.cpp
        src/natives/ip.cpp
        src/natives/sockets.cpp
        src/natives/enet.cpp
        src/natives/websocket.cpp
        src/natives/aes.cpp
        src/natives/pack.cpp
)

# Slice 1: optional `sdl` native sources. Linked into zym when ZYM_SDL=ON.
if(ZYM_SDL)
    list(APPEND ZYM_NATIVE_SOURCES src/natives/ui/sdl.cpp)
endif()

# Slice 1 PR 2: optional `ui` native sources. Linked into zym when
# ZYM_UI=ON (which implies ZYM_SDL=ON; see the soft-dep check above).
# Split across src/natives/ui/:
#   * ui.cpp     — thin FFI binder + per-window context lifecycle
#   * imgui.cpp  — every `u_*` ImGui wrapper + registerImGuiBindings()
#   * implot.cpp — every `u_plot*` ImPlot wrapper + registerImPlotBindings()
#                  (placeholder right now; Phase A populates it)
if(ZYM_UI)
    list(APPEND ZYM_NATIVE_SOURCES
            src/natives/ui/ui_internal.hpp
            src/natives/ui/ui.cpp
            src/natives/ui/imgui.cpp
            src/natives/ui/implot.cpp
            src/natives/ui/imanim.cpp)
endif()

# src/boot/register_core.cpp invokes ObjectDB::setup / cleanup and
# StringName::setup / cleanup, which the engine declares `private` and
# friends only to godot/core/register_core_types.cpp. We can't modify
# godot/ and `#define private public` is not reliable here, so disable
# access checks for this TU only. See src/boot/register_core.cpp for
# the rationale.
set_source_files_properties(src/boot/register_core.cpp
        PROPERTIES COMPILE_OPTIONS "-fno-access-control"
)

add_executable(zym ${APP_SOURCES} ${ZYM_NATIVE_SOURCES})
if(ZYM_LTO_OK)
    set_property(TARGET zym PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET zym PROPERTY INTERPROCEDURAL_OPTIMIZATION_TINY TRUE)
endif()

target_link_libraries(zym PRIVATE zym_core)

# --- SDL3 (Slice 1) ------------------------------------------------------
# Vendored under third_party/sdl3 (git submodule, pinned to release-3.4.8).
# Build SDL3 as a static library with subsystems trimmed to the bare
# minimum Slice 1 needs (video + events). Everything else (audio,
# joystick, haptic, sensor, camera, hidapi, power, dialog, process) is
# OFF so it doesn't bloat the binary and doesn't drag in extra deps.
# Renderer drivers stay enabled because Slice 1 hands ImGui an
# SDL_Renderer to draw into (PR 2). GPU/Vulkan/Metal/D3D12 stay off
# (they're a later slice).
if(ZYM_SDL)
    set(SDL_STATIC      ON  CACHE BOOL "" FORCE)
    set(SDL_SHARED      OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS       OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES    OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL     OFF CACHE BOOL "" FORCE)
    set(SDL_DISABLE_INSTALL_DOCS ON CACHE BOOL "" FORCE)
    # Subsystems: keep video + events (and the renderer subsystem so
    # SDL_CreateRenderer / SDL_SetRenderVSync work). Drop the rest.
    set(SDL_AUDIO       OFF CACHE BOOL "" FORCE)
    set(SDL_JOYSTICK    OFF CACHE BOOL "" FORCE)
    set(SDL_HAPTIC      OFF CACHE BOOL "" FORCE)
    set(SDL_HIDAPI      OFF CACHE BOOL "" FORCE)
    set(SDL_POWER       OFF CACHE BOOL "" FORCE)
    set(SDL_SENSOR      OFF CACHE BOOL "" FORCE)
    set(SDL_CAMERA      OFF CACHE BOOL "" FORCE)
    set(SDL_DIALOG      OFF CACHE BOOL "" FORCE)
    set(SDL_PROCESS     OFF CACHE BOOL "" FORCE)
    set(SDL_GPU         OFF CACHE BOOL "" FORCE)
    set(SDL_VULKAN      OFF CACHE BOOL "" FORCE)
    set(SDL_METAL       OFF CACHE BOOL "" FORCE)
    set(SDL_OPENVR      OFF CACHE BOOL "" FORCE)
    # Slice 1 uses imgui_impl_sdlrenderer3 (PR 2). Keep the renderer
    # subsystem; OpenGL is enough as a portable software-or-GL fallback.
    set(SDL_RENDER      ON  CACHE BOOL "" FORCE)
    set(SDL_OPENGL      ON  CACHE BOOL "" FORCE)
    # Trim renderers/video drivers we don't ship in Slice 1:
    #   * Dummy + offscreen video drivers (~150-200 KB) -- we always have
    #     a real display when the `sdl` native is in use; headless callers
    #     simply don't import `sdl`.
    #   * OpenGL ES2 renderer (~100-200 KB) -- desktop OpenGL is what
    #     imgui_impl_sdlrenderer3 will draw through; ES2 is redundant.
    #   * Audio backends (Pipewire/Pulse/ALSA/Jack/sndio/OSS): SDL_AUDIO is
    #     already OFF above, but the dep_option machinery can still pull
    #     the backend stubs in via defaults. Force every Linux audio
    #     backend OFF explicitly so no stub code survives gc-sections.
    set(SDL_VIDEO_DUMMY     OFF CACHE BOOL "" FORCE)
    set(SDL_DUMMYVIDEO      OFF CACHE BOOL "" FORCE)
    set(SDL_VIDEO_OFFSCREEN OFF CACHE BOOL "" FORCE)
    set(SDL_OFFSCREEN       OFF CACHE BOOL "" FORCE)
    set(SDL_DUMMYAUDIO      OFF CACHE BOOL "" FORCE)
    set(SDL_OPENGLES        OFF CACHE BOOL "" FORCE)
    set(SDL_RENDER_GPU      OFF CACHE BOOL "" FORCE)
    set(SDL_PIPEWIRE        OFF CACHE BOOL "" FORCE)
    set(SDL_PULSEAUDIO      OFF CACHE BOOL "" FORCE)
    set(SDL_ALSA            OFF CACHE BOOL "" FORCE)
    set(SDL_JACK            OFF CACHE BOOL "" FORCE)
    set(SDL_SNDIO           OFF CACHE BOOL "" FORCE)
    set(SDL_OSS             OFF CACHE BOOL "" FORCE)
    # X11 sub-features: drop the optional ones whose dev headers aren't
    # universally available on minimal Linux hosts. Core X11 stays on
    # (SDL needs it for the basic window).
    set(SDL_X11_XSCRNSAVER OFF CACHE BOOL "" FORCE)
    set(SDL_X11_XINPUT     OFF CACHE BOOL "" FORCE)
    set(SDL_X11_XSYNC      OFF CACHE BOOL "" FORCE)
    set(SDL_X11_XTEST      OFF CACHE BOOL "" FORCE)
    set(SDL_X11_XDBE       OFF CACHE BOOL "" FORCE)
    set(SDL_X11_XSHAPE     OFF CACHE BOOL "" FORCE)
    set(SDL_IBUS           OFF CACHE BOOL "" FORCE)
    set(SDL_DBUS           OFF CACHE BOOL "" FORCE)
    set(SDL_LIBURING       OFF CACHE BOOL "" FORCE)
    # Don't propagate SDL3's warnings/options into the zym TUs.
    add_subdirectory(third_party/sdl3 EXCLUDE_FROM_ALL)
    # Propagate the same size-tuning treatment we apply to `zym` over to
    # SDL3-static so we're comparing apples-to-apples in Release/Tiny:
    #   1. LTO. CMake's `INTERPROCEDURAL_OPTIMIZATION_<CFG>` is set per-target,
    #      not per-project, so flipping it on `zym` (lines 159-162 above) does
    #      NOT propagate to SDL3-static. Without this, every SDL3 TU is
    #      optimised in isolation -- no cross-TU inlining, no cross-TU DCE
    #      inside reachable functions (the dispatch switches in SDL_CreateWindow
    #      that select between video drivers stay fully populated even when
    #      only x11 is registered). Turning IPO on for SDL3-static lets the
    #      linker fold the dead branches and inline the small helpers.
    #   2. `-fno-pie`. `zym` is linked `-no-pie` (libgodot.a is non-PIC), so
    #      SDL3-static needs to be position-dependent too or the final link
    #      mixes PIC and non-PIC objects.
    #   3. `-flto` mode. Match zym's `-flto` (Release single-partition) /
    #      `-flto=auto` (other configs). Required for LTO IR compatibility
    #      with the rest of the link.
    if(ZYM_LTO_OK)
        set_property(TARGET SDL3-static PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set_property(TARGET SDL3-static PROPERTY INTERPROCEDURAL_OPTIMIZATION_TINY TRUE)
    endif()
    target_compile_options(SDL3-static PRIVATE
            $<IF:$<CONFIG:Release>,-flto,-flto=auto>)
    if(NOT WIN32)
        target_compile_options(SDL3-static PRIVATE -fno-pie)
    endif()
    target_link_libraries(zym PRIVATE SDL3::SDL3-static)
    target_compile_definitions(zym PRIVATE ZYM_SDL_ENABLED=1)
    message(STATUS ">> ZYM_SDL: ON — vendored SDL3 (video+events+render) linked into zym")
    if(ZYM_LTO_OK)
        message(STATUS ">> ZYM_SDL: LTO + gc-sections + Release flags propagated to SDL3-static")
    endif()
else()
    message(STATUS ">> ZYM_SDL: OFF — `sdl` native disabled")
endif()

# --- SDL_image (Slice 2 PR 5) -------------------------------------------
# Vendored under third_party/sdl_image (git submodule, pinned to
# release-3.4.4 — the SDL3-API-compatible major). Built as a small
# static library `SDL3_image-static` and linked into zym when ZYM_SDL=ON.
# This is the image-decoding satellite library; SDL3 core itself only
# ships a built-in BMP loader (SDL_LoadBMP) — every other format
# (PNG, JPG, ...) flows through SDL_image.
#
# Format trim per Slice 2 §2.0:
#   * PNG: ON  (libpng-vendored, includes save support)
#   * JPG: ON  (stb_image backend — header-only, no vendored libjpeg)
#   * BMP: OFF (routed through SDL3 core's SDL_LoadBMP / SDL_SaveBMP)
#   * Everything else (AVIF, GIF, JXL, LBM, PCX, PNM, QOI, SVG, TGA,
#     TIF, WEBP, XCF, XPM, XV, ANI): OFF
# This keeps the added size to libpng + zlib + the SDL_image dispatch
# core (~250-400 KB target).
#
# No new CMake flag and no new catalog row: Surface / Texture /
# UI.image* all ride on ZYM_SDL + ZYM_UI. A "SDL+UI but no image
# support" configuration is a shape nobody asked for — see §2.0.
if(ZYM_SDL)
    # Build mode + install behaviour. Force a static build of
    # SDL_image (matches SDL3-static); SDL_image's CMakeLists picks
    # the shared vs static branch from the global BUILD_SHARED_LIBS,
    # which CMake's cmake_dependent_option defaults to ON on
    # Linux/macOS — wrong for us. Override it explicitly so
    # SDL3_image-static is the target produced.
    set(BUILD_SHARED_LIBS   OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_INSTALL    OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_SAMPLES    OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_TESTS      OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_VENDORED   ON  CACHE BOOL "" FORCE)
    set(SDLIMAGE_DEPS_SHARED OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_RELOCATABLE OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_WERROR     OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_STRICT     OFF CACHE BOOL "" FORCE)
    # Backends. STB on for JPEG (header-only, no libjpeg-turbo
    # vendored). WIC / ImageIO are gated by platform; explicit OFF
    # so nothing platform-conditional sneaks in.
    set(SDLIMAGE_BACKEND_STB     ON  CACHE BOOL "" FORCE)
    set(SDLIMAGE_BACKEND_WIC     OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_BACKEND_IMAGEIO OFF CACHE BOOL "" FORCE)
    # Format loaders: PNG + JPG only.
    set(SDLIMAGE_PNG  ON  CACHE BOOL "" FORCE)
    set(SDLIMAGE_JPG  ON  CACHE BOOL "" FORCE)
    set(SDLIMAGE_BMP  OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_ANI  OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_AVIF OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_GIF  OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_JXL  OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_LBM  OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_PCX  OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_PNM  OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_QOI  OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_SVG  OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_TGA  OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_TIF  OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_WEBP OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_XCF  OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_XPM  OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_XV   OFF CACHE BOOL "" FORCE)
    # libpng for PNG (required for APNG too, but APNG is a free
    # bonus since libpng handles it natively).
    set(SDLIMAGE_PNG_LIBPNG ON CACHE BOOL "" FORCE)
    # Save support: PNG + JPG (the two formats we load).
    set(SDLIMAGE_PNG_SAVE ON CACHE BOOL "" FORCE)
    set(SDLIMAGE_JPG_SAVE ON CACHE BOOL "" FORCE)
    # Other *_SAVE knobs are dependent options gated on their
    # loader being ON; with everything else OFF above they're
    # already OFF and don't need explicit settings.
    add_subdirectory(third_party/sdl_image EXCLUDE_FROM_ALL)
    # SDL_image exports SDL3_image-static when BUILD_SHARED_LIBS=OFF
    # (which we force above before add_subdirectory). Match the LTO
    # / -fno-pie / -flto-mode propagation we do for SDL3-static so
    # the final link is consistent.
    if(TARGET SDL3_image-static)
        set(ZYM_SDL_IMAGE_TARGET SDL3_image-static)
    elseif(TARGET SDL3_image-shared)
        set(ZYM_SDL_IMAGE_TARGET SDL3_image-shared)
    else()
        message(FATAL_ERROR ">> ZYM_SDL: SDL_image did not produce a SDL3_image-static or -shared target")
    endif()
    if(ZYM_LTO_OK)
        set_property(TARGET ${ZYM_SDL_IMAGE_TARGET} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set_property(TARGET ${ZYM_SDL_IMAGE_TARGET} PROPERTY INTERPROCEDURAL_OPTIMIZATION_TINY    TRUE)
    endif()
    target_compile_options(${ZYM_SDL_IMAGE_TARGET} PRIVATE
            $<IF:$<CONFIG:Release>,-flto,-flto=auto>)
    if(NOT WIN32)
        target_compile_options(${ZYM_SDL_IMAGE_TARGET} PRIVATE -fno-pie)
    endif()
    # SDL_image's vendored libpng / zlib / stb TUs trip the same
    # warnings as SDL3 itself; silence them so they don't drown out
    # our own. Done as a private option on the static target.
    target_compile_options(${ZYM_SDL_IMAGE_TARGET} PRIVATE -w)
    target_link_libraries(zym PRIVATE ${ZYM_SDL_IMAGE_TARGET})
    target_compile_definitions(zym PRIVATE ZYM_SDL_IMAGE_ENABLED=1)
    message(STATUS ">> ZYM_SDL: SDL_image (release-3.4.4, PNG+JPG only) linked into zym via ${ZYM_SDL_IMAGE_TARGET}")
    if(ZYM_LTO_OK)
        message(STATUS ">> ZYM_SDL: LTO + Release flags propagated to ${ZYM_SDL_IMAGE_TARGET}")
    endif()
endif()

# --- Dear ImGui (Slice 1 PR 2) ------------------------------------------
# Vendored under third_party/imgui (git submodule, pinned to v1.92.8).
# Built as a small static library `zym_imgui` so ImGui's compile flags
# (and the SDL3 include path it needs for the backend TUs) don't leak
# into the rest of the zym translation units.
#
# Sources: ImGui core (imgui*.cpp), plus the two upstream SDL3 backends
# (imgui_impl_sdl3.cpp + imgui_impl_sdlrenderer3.cpp). Demo / freetype
# / other addons are intentionally NOT included — Slice 1 ships only
# the core widget surface.
if(ZYM_UI)
    set(ZYM_IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui")
    add_library(zym_imgui STATIC
            "${ZYM_IMGUI_DIR}/imgui.cpp"
            "${ZYM_IMGUI_DIR}/imgui_draw.cpp"
            "${ZYM_IMGUI_DIR}/imgui_tables.cpp"
            "${ZYM_IMGUI_DIR}/imgui_widgets.cpp"
            "${ZYM_IMGUI_DIR}/imgui_demo.cpp"
            "${ZYM_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
            "${ZYM_IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp"
    )
    target_include_directories(zym_imgui PUBLIC
            "${ZYM_IMGUI_DIR}"
            "${ZYM_IMGUI_DIR}/backends"
    )
    # The SDL3 backends need <SDL3/SDL.h>; pull the include dir from
    # the static target so we don't hardcode the source-tree path.
    target_link_libraries(zym_imgui PUBLIC SDL3::SDL3-static)
    # Same propagation treatment as SDL3-static: match zym's LTO mode,
    # match -fno-pie for the libgodot.a non-PIC link.
    if(ZYM_LTO_OK)
        set_property(TARGET zym_imgui PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set_property(TARGET zym_imgui PROPERTY INTERPROCEDURAL_OPTIMIZATION_TINY TRUE)
    endif()
    target_compile_options(zym_imgui PRIVATE
            $<IF:$<CONFIG:Release>,-flto,-flto=auto>)
    if(NOT WIN32)
        target_compile_options(zym_imgui PRIVATE -fno-pie)
    endif()
    # ImGui internally uses lots of `(void)var` and switch-case fallthrough
    # patterns its own headers silence; the zym build is fussier (Wall in
    # debug, -fno-exceptions). Silence warnings inside the ImGui TUs only
    # so they don't drown out our own.
    target_compile_options(zym_imgui PRIVATE -w)
    target_link_libraries(zym PRIVATE zym_imgui)
    target_compile_definitions(zym PRIVATE ZYM_UI_ENABLED=1)
    message(STATUS ">> ZYM_UI: ON — vendored ImGui (v1.92.8) linked into zym via zym_imgui")
    if(ZYM_LTO_OK)
        message(STATUS ">> ZYM_UI: LTO + Release flags propagated to zym_imgui")
    endif()
else()
    message(STATUS ">> ZYM_UI: OFF — `ui` native disabled")
endif()

# --- ImPlot (Slice 1 PR 2d) ---------------------------------------------
# Vendored under third_party/implot (git submodule, pinned to v1.0).
# Built as a small static library `zym_implot` that links PUBLICLY
# against `zym_imgui` (ImPlot is a pure ImGui addon — it draws via
# ImDrawList and inherits the SDL3 include path / backend the rest of
# the UI stack already uses).
#
# Sources: ImPlot core (implot.cpp) + items (implot_items.cpp).
# implot_demo.cpp is intentionally omitted — Slice 1 PR 2d ships only
# the plotting surface, no demo gallery (matches how we handle
# imgui_demo.cpp's exclusion would-be-but-isn't, see note above:
# we currently DO ship imgui_demo.cpp for parity with widgets; the
# ImPlot demo is much larger (~200 KB) and not wired through `ui.*`).
if(ZYM_UI)
    set(ZYM_IMPLOT_DIR "${CMAKE_SOURCE_DIR}/third_party/implot")
    add_library(zym_implot STATIC
            "${ZYM_IMPLOT_DIR}/implot.cpp"
            "${ZYM_IMPLOT_DIR}/implot_items.cpp"
    )
    target_include_directories(zym_implot PUBLIC "${ZYM_IMPLOT_DIR}")
    # PUBLIC link to zym_imgui propagates ImGui + SDL3 includes to any
    # TU that pulls in <implot.h>.
    target_link_libraries(zym_implot PUBLIC zym_imgui)
    # Same LTO / -fno-pie / -flto-mode propagation as zym_imgui.
    if(ZYM_LTO_OK)
        set_property(TARGET zym_implot PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set_property(TARGET zym_implot PROPERTY INTERPROCEDURAL_OPTIMIZATION_TINY TRUE)
    endif()
    target_compile_options(zym_implot PRIVATE
            $<IF:$<CONFIG:Release>,-flto,-flto=auto>)
    if(NOT WIN32)
        target_compile_options(zym_implot PRIVATE -fno-pie)
    endif()
    # Silence ImPlot's own warnings the same way we do for ImGui.
    target_compile_options(zym_implot PRIVATE -w)
    target_link_libraries(zym PRIVATE zym_implot)
    target_compile_definitions(zym PRIVATE ZYM_IMPLOT_ENABLED=1)
    message(STATUS ">> ZYM_UI: ON — vendored ImPlot (v1.0) linked into zym via zym_implot")
    if(ZYM_LTO_OK)
        message(STATUS ">> ZYM_UI: LTO + Release flags propagated to zym_implot")
    endif()
endif()

# --- ImAnim (Slice 1 PR 2e) ---------------------------------------------
# Vendored under third_party/imanim (git submodule, pinned to tag 1.0.0,
# sha 8a537521ba41b3dfeec5173f2f89a8dc3d6387c8). Built as a small static
# library `zym_imanim` that links PUBLICLY against `zym_imgui` (ImAnim
# is a pure ImGui addon — its public header `im_anim.h` includes
# `imgui.h` directly, so the include path / SDL3 transitive surface
# already provided by zym_imgui is exactly what it needs).
#
# Sources: ImAnim core only (im_anim.cpp). The upstream tag also ships
# `im_anim_demo.cpp`, `im_anim_doc.cpp`, and `im_anim_usecase.cpp`
# (~820 KB combined) which are reference/demo TUs — intentionally not
# compiled here. Zym-side `UI.anim*` bindings are deferred (this PR
# only ensures the vendor compile and link succeeds).
if(ZYM_UI)
    set(ZYM_IMANIM_DIR "${CMAKE_SOURCE_DIR}/third_party/imanim")
    add_library(zym_imanim STATIC
            "${ZYM_IMANIM_DIR}/im_anim.cpp"
    )
    target_include_directories(zym_imanim PUBLIC "${ZYM_IMANIM_DIR}")
    # PUBLIC link to zym_imgui propagates ImGui includes to any TU
    # that pulls in <im_anim.h>.
    target_link_libraries(zym_imanim PUBLIC zym_imgui)
    # Same LTO / -fno-pie / -flto-mode propagation as zym_implot.
    if(ZYM_LTO_OK)
        set_property(TARGET zym_imanim PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set_property(TARGET zym_imanim PROPERTY INTERPROCEDURAL_OPTIMIZATION_TINY TRUE)
    endif()
    target_compile_options(zym_imanim PRIVATE
            $<IF:$<CONFIG:Release>,-flto,-flto=auto>)
    if(NOT WIN32)
        target_compile_options(zym_imanim PRIVATE -fno-pie)
    endif()
    # Silence ImAnim's own warnings the same way we do for ImGui / ImPlot.
    target_compile_options(zym_imanim PRIVATE -w)
    target_link_libraries(zym PRIVATE zym_imanim)
    target_compile_definitions(zym PRIVATE ZYM_IMANIM_ENABLED=1)
    message(STATUS ">> ZYM_UI: ON — vendored ImAnim (1.0.0) linked into zym via zym_imanim")
    if(ZYM_LTO_OK)
        message(STATUS ">> ZYM_UI: LTO + Release flags propagated to zym_imanim")
    endif()
endif()

if(RUNTIME_ONLY)
    message(STATUS ">> RUNTIME_ONLY: ON - excluding full_executor; runtime_loader path only")
    target_compile_definitions(zym PRIVATE ZYM_RUNTIME_ONLY=1)
    # Emit the runtime-only binary into cli/runtimes/ with a platform name
    # the loader can pick up: `windows.exe` on Windows, `linux` elsewhere.
    set_target_properties(zym PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/cli/runtimes"
    )
    foreach(_cfg "" _DEBUG _RELEASE _TINY _RELWITHDEBINFO _MINSIZEREL)
        set_target_properties(zym PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY${_cfg} "${CMAKE_SOURCE_DIR}/cli/runtimes"
        )
    endforeach()
    if(WIN32)
        set_target_properties(zym PROPERTIES OUTPUT_NAME "windows" SUFFIX ".exe")
    else()
        set_target_properties(zym PROPERTIES OUTPUT_NAME "linux" SUFFIX "")
    endif()
else()
    target_compile_definitions(zym PRIVATE ZYM_RUNTIME_ONLY=0)
endif()

if(USE_ENGINE_ERRORS)
    target_compile_definitions(zym PRIVATE ZYM_ENGINE_ERRORS=1)
else()
    target_compile_definitions(zym PRIVATE ZYM_ENGINE_ERRORS=0)
endif()

# Godot static library integration.
# ZYM_PRECOMPILE_GODOT: ON runs the platform script to build libgodot.a; OFF expects it on disk.
# ZYM_GODOT_PLATFORM: selects script/artifact. Supported: linux_x86_64.
# Plain (non-cache) vars - this file is the source of truth.
set(ZYM_PRECOMPILE_GODOT OFF)
# Auto-select platform from the CMake toolchain. Override by setting
# ZYM_GODOT_PLATFORM before this point if cross-targeting something else.
if(NOT DEFINED ZYM_GODOT_PLATFORM)
    if(WIN32)
        set(ZYM_GODOT_PLATFORM "windows_x86_64")
    else()
        set(ZYM_GODOT_PLATFORM "linux_x86_64")
    endif()
endif()

set(ZYM_GODOT_DIR     "${CMAKE_SOURCE_DIR}/godot")
set(ZYM_GODOT_BIN_DIR "${ZYM_GODOT_DIR}/bin")

if(ZYM_GODOT_PLATFORM STREQUAL "linux_x86_64")
    set(ZYM_GODOT_LIB_FILE   "libgodot.linuxbsd.template_release.x86_64.a")
    set(ZYM_GODOT_BUILD_SH   "${CMAKE_SOURCE_DIR}/scripts/build_godot_linux_x86_64.sh")
elseif(ZYM_GODOT_PLATFORM STREQUAL "windows_x86_64")
    set(ZYM_GODOT_LIB_FILE   "libgodot.windows.template_release.x86_64.a")
    set(ZYM_GODOT_BUILD_SH   "${CMAKE_SOURCE_DIR}/scripts/build_godot_windows_x86_64.sh")
else()
    message(FATAL_ERROR "ZYM_GODOT_PLATFORM='${ZYM_GODOT_PLATFORM}' is not supported yet.")
endif()

set(ZYM_GODOT_LIB_PATH "${ZYM_GODOT_BIN_DIR}/${ZYM_GODOT_LIB_FILE}")

# libgodot.<suffix>.a is just the platform entry-point; real symbols live in
# per-subsystem archives under godot/bin/obj/<subsystem>/. Link them all
# (core last so forward-refs resolve).
if(ZYM_GODOT_PLATFORM STREQUAL "linux_x86_64")
    set(ZYM_GODOT_LIB_SUFFIX "linuxbsd.template_release.x86_64")
elseif(ZYM_GODOT_PLATFORM STREQUAL "windows_x86_64")
    set(ZYM_GODOT_LIB_SUFFIX "windows.template_release.x86_64")
endif()
set(ZYM_GODOT_SUBLIBS
        "${ZYM_GODOT_BIN_DIR}/obj/platform/libplatform.${ZYM_GODOT_LIB_SUFFIX}.a"
        "${ZYM_GODOT_BIN_DIR}/obj/drivers/libdrivers.${ZYM_GODOT_LIB_SUFFIX}.a"
        "${ZYM_GODOT_BIN_DIR}/obj/modules/libmodules.${ZYM_GODOT_LIB_SUFFIX}.a"
        "${ZYM_GODOT_BIN_DIR}/obj/modules/libmodule_regex.${ZYM_GODOT_LIB_SUFFIX}.a"
        "${ZYM_GODOT_BIN_DIR}/obj/modules/libmodule_mbedtls.${ZYM_GODOT_LIB_SUFFIX}.a"
        "${ZYM_GODOT_BIN_DIR}/obj/modules/libmodule_enet.${ZYM_GODOT_LIB_SUFFIX}.a"
        "${ZYM_GODOT_BIN_DIR}/obj/modules/libmodule_websocket.${ZYM_GODOT_LIB_SUFFIX}.a"
        "${ZYM_GODOT_BIN_DIR}/obj/core/libcore.${ZYM_GODOT_LIB_SUFFIX}.a"
)

if(ZYM_PRECOMPILE_GODOT)
    message(STATUS ">> Godot: precompile ENABLED via ${ZYM_GODOT_BUILD_SH}")
    add_custom_command(
            OUTPUT  "${ZYM_GODOT_LIB_PATH}"
            COMMAND "${ZYM_GODOT_BUILD_SH}"
            WORKING_DIRECTORY "${ZYM_GODOT_DIR}"
            COMMENT "Building Godot static library (${ZYM_GODOT_PLATFORM}) via SCons"
            VERBATIM
    )
    add_custom_target(godot_static_lib DEPENDS "${ZYM_GODOT_LIB_PATH}")
    # Mark as GENERATED so Ninja links it to the custom-command rule.
    set_source_files_properties("${ZYM_GODOT_LIB_PATH}" PROPERTIES GENERATED TRUE)
else()
    message(STATUS ">> Godot: precompile DISABLED (expecting ${ZYM_GODOT_LIB_PATH})")
    add_custom_target(godot_static_lib)
endif()

# No IMPORTED target: --whole-archive requires naming the .a inline between
# the whole/no-whole flags, which an IMPORTED_LOCATION would defeat.
# --whole-archive is required so ClassDB static self-registrations survive
# dead-code elimination.
add_dependencies(zym godot_static_lib)
if(WIN32)
    set(ZYM_GODOT_PLATFORM_INCLUDE "${ZYM_GODOT_DIR}/platform/windows")
else()
    set(ZYM_GODOT_PLATFORM_INCLUDE "${ZYM_GODOT_DIR}/platform/linuxbsd")
endif()
target_include_directories(zym PRIVATE
        "${ZYM_GODOT_DIR}"
        "${ZYM_GODOT_PLATFORM_INCLUDE}"
        # mbedtls thirdparty headers (`mbedtls/*.h`, `psa/*.h`) are needed by
        # `src/boot/register_core.cpp` to bring up the mbedtls-backed crypto
        # factories without going through the engine's `GLOBAL_DEF`-using
        # registration wrapper.
        "${ZYM_GODOT_DIR}/thirdparty/mbedtls/include"
        # enet thirdparty headers (`<enet/enet.h>`) are needed by
        # `godot/modules/enet/enet_packet_peer.h` and `enet_connection.h`,
        # which are themselves transitively included by `src/natives/enet.cpp`.
        "${ZYM_GODOT_DIR}/thirdparty/enet"
)
if(WIN32)
    # On Linux we'd just use the system zlib via /usr/include, but the
    # mingw-w64 sysroot has no zlib.h, so point at godot's bundled copy.
    # `core/io/compression.h` (transitively included by `core/io/file_access.h`)
    # does `#include <zlib.h>` unconditionally.
    target_include_directories(zym PRIVATE
            "${ZYM_GODOT_DIR}/thirdparty/zlib"
    )
endif()
# Must mirror the SCons CPPDEFINES that built libgodot.a or the header
# virtual surface drifts and overrides fail to match. See godot/SConstruct.
target_compile_definitions(zym PRIVATE
        # Match the mbedTLS configuration the static archive was built
        # with (godot/modules/mbedtls/SCsub). This in turn defines
        # `MBEDTLS_THREADING_ALT` (under THREADS_ENABLED), which
        # `src/boot/register_core.cpp` keys off to install the
        # `godot_mbedtls_mutex_*` callbacks. Without this define,
        # mbedtls headers see the stock config and the boot code never
        # calls `mbedtls_threading_set_alt`, causing CTR_DRBG seeding to
        # fail with `MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED`.
        "MBEDTLS_CONFIG_FILE=\"thirdparty/mbedtls/include/godot_module_mbedtls_config.h\""
        DISABLE_DEPRECATED
        LIBGODOT_ENABLED
        _3D_DISABLED
        ADVANCED_GUI_DISABLED
        PHYSICS_2D_DISABLED
        PHYSICS_3D_DISABLED
        NAVIGATION_2D_DISABLED
        NAVIGATION_3D_DISABLED
        XR_DISABLED
        THREADS_ENABLED
)
if(WIN32)
    # From godot/platform/windows/detect.py (mingw path).
    target_compile_definitions(zym PRIVATE
            WINDOWS_ENABLED
            WINVER=0x0A00
            _WIN32_WINNT=0x0A00
            NOMINMAX
            _WIN64
            MINGW_ENABLED
            MINGW_HAS_SECURE_API=1
            # zym: tell `OS_Windows::OS_Windows()` to skip `RedirectIOToConsole()`.
            # Without this, the ctor calls `AttachConsole(ATTACH_PARENT_PROCESS)`
            # + three `freopen_s(...)` redirections (CONIN$/CONOUT$ for
            # stdin/stdout/stderr) on every boot. That path exists for windowed
            # (subsystem:windows) builds that need to attach to the parent
            # console at runtime. We link with `-mconsole` (PE subsystem 3 =
            # console), so the loader already attaches a console for us before
            # the binary starts; `AttachConsole(ATTACH_PARENT_PROCESS)` then
            # returns `ERROR_ACCESS_DENIED` (already attached) and the redirections
            # are pure wasted Win32/Wine syscalls -- noticeable on Wine where
            # AttachConsole + freopen_s round-trip through wineserver.
            WINDOWS_SUBSYSTEM_CONSOLE
    )
else()
    # From godot/platform/linuxbsd/detect.py (unconditional for linuxbsd):
    target_compile_definitions(zym PRIVATE
            LINUXBSD_ENABLED
            UNIX_ENABLED
            _FILE_OFFSET_BITS=64
    )
endif()
# -flto: required because libgodot.a was built with `lto=full` (stores GCC
# LTO IR, not native objects). Full (single-partition) in Release for max
# cross-module inlining/DCE; auto (parallel) elsewhere for faster links.
# -fno-pie/-no-pie: libgodot.a is non-PIC (static_library omits -fPIC); match
# it to avoid DT_TEXTREL in a PIE. PE/COFF (mingw) has no PIE/TEXTREL concept,
# so the flags are linux-only -- including them on mingw triggers
# `-fno-pie ignored for target (all code is position independent)` chatter
# at best, or link errors against missing crt objects at worst.
target_compile_options(zym PRIVATE
        $<IF:$<CONFIG:Release>,-flto,-flto=auto>)
target_link_options(zym PRIVATE
        $<IF:$<CONFIG:Release>,-flto,-flto=auto>)
if(NOT WIN32)
    target_compile_options(zym PRIVATE -fno-pie)
    target_link_options(zym PRIVATE -no-pie)
endif()
if(WIN32)
    # Note: a previous iteration added `-fuse-linker-plugin` here to
    # consume pure LTO IR from libgodot.windows.a. That approach was
    # reverted because Debian/Ubuntu's `x86_64-w64-mingw32-gcc-ar` does
    # not load `liblto_plugin.so`, so slim-LTO archives have an empty
    # symtab and the final link fails with thousands of undefined
    # references. Godot's `detect.py` therefore stays on the upstream
    # `-fno-use-linker-plugin -fwhole-program` mingw-gcc workaround
    # (fat LTO objects), and we do not pass `-fuse-linker-plugin` to
    # the final zym link either.
    # CLI binary: force the console subsystem so stdio works when launched
    # from cmd.exe / PowerShell / a pipe. Without this, mingw defaults to
    # whatever the spec file picks (usually console for non-GUI, but be
    # explicit).
    target_link_options(zym PRIVATE -mconsole)
    # MinGW-w64's posix threading model makes libstdc++ depend on
    # libwinpthread-1.dll at runtime. -static-libstdc++/-static-libgcc do
    # NOT cover winpthread, so without this the produced zym.exe fails to
    # start under wine / on bare Windows with:
    #   err:module:import_dll Library libwinpthread-1.dll ... not found
    # `-static` forces every remaining MinGW runtime (winpthread, and any
    # other implicit libs) to be linked statically, producing a fully
    # self-contained PE. Safe because we have no intentional DLL deps
    # other than the Win32 system libs (ws2_32, kernel32, ...), which are
    # import libraries and unaffected by -static.
    target_link_options(zym PRIVATE -static)
endif()

# Identical Code Folding via gold. Folds bytewise-identical functions emitted
# as separate symbols -- in this build that's the long tail of vtable-pinned
# ClassDB reflection thunks (`*::_property_can_revertv`,
# `_property_get_revertv`, `_validate_propertyv`, `_notification_backwardv`)
# and the duplicated template instantiations of `Ref<>::~Ref()`,
# `CowData<>::_ref()`, `_convert_array<>()`, `Variant::PackedArrayRef<>::~PackedArrayRef()`,
# `HashMap<>::~HashMap()`, etc. that LTO emits per partition. `--icf=safe`
# only folds functions whose addresses aren't taken (safe under C++ identity
# rules), so it can't break vtable dispatch or function-pointer comparison.
#
# We deliberately use gold (not lld) here. `libgodot.a` is built with GCC
# `lto=full`, and our own TUs are compiled with `-flto` under GCC, so every
# input `.o` carries GCC LTO IR in `.gnu.lto_*` sections rather than native
# code. Only `ld.bfd` and `ld.gold` understand the GCC LTO plugin protocol
# (`liblto_plugin.so`); `ld.lld` cannot parse GCC bitcode and reports the
# IR-only TUs' contents as undefined (e.g. `undefined symbol: main` from
# crt1.o referencing a `main` that lives only in IR). Plain BFD ld would
# work but doesn't implement ICF, so gold is the only linker that gives us
# both the GCC LTO pipeline and `--icf=safe`.
# gold is ELF-only -- it cannot emit PE/COFF, so on mingw we just stay on the
# default BFD ld (which on mingw-w64 does understand the GCC LTO plugin) and
# accept losing --icf=safe. The Linux rationale below still applies.
if(WIN32)
    # ICF on mingw: ld.gold is ELF-only and BFD ld doesn't implement ICF,
    # so the only candidate is lld (PE/COFF backend). Note: since the
    # `-fuse-linker-plugin` attempt was reverted (see WIN32 block above),
    # libgodot.windows.a contains FAT LTO objects (native code + GCC LTO
    # IR side by side). lld's COFF backend reads the native half and
    # ignores `.gnu.lto_*` sections, so ICF can fold native duplicates
    # but won't perform whole-program LTO. That's still a net win.
    #
    # We probe for an lld variant that the mingw-gcc driver will accept
    # via `-fuse-ld=lld`: a cross-prefixed `x86_64-w64-mingw32-ld.lld`,
    # the generic `ld.lld`, or `lld` on PATH. If none is found, we stay
    # on BFD ld and accept the loss of ICF (BFD has no equivalent flag).
    # `--icf=safe` is GNU-style and is accepted by lld on both ELF and
    # PE/COFF; lld translates it internally to the `/opt:icf` semantic
    # on COFF. Only address-not-taken functions are folded, so vtable
    # dispatch and function-pointer identity are preserved.
    find_program(ZYM_LLD_EXECUTABLE
            NAMES x86_64-w64-mingw32-ld.lld ld.lld lld)
    if(ZYM_LLD_EXECUTABLE)
        message(STATUS ">> ICF: using ${ZYM_LLD_EXECUTABLE} (-fuse-ld=lld -Wl,--icf=safe)")
        target_link_options(zym PRIVATE
                -fuse-ld=lld
                "-Wl,--icf=safe"
        )
    else()
        message(STATUS ">> ICF: no ld.lld found; staying on mingw BFD ld (no --icf=safe; BFD has no ICF, gold is ELF-only)")
    endif()
else()
    find_program(ZYM_GOLD_EXECUTABLE NAMES ld.gold gold)
    if(ZYM_GOLD_EXECUTABLE)
        message(STATUS ">> ICF: using ${ZYM_GOLD_EXECUTABLE} (-fuse-ld=gold -Wl,--icf=safe)")
        target_link_options(zym PRIVATE
                -fuse-ld=gold
                "-Wl,--icf=safe"
        )
    else()
        message(STATUS ">> ICF: no ld.gold found; skipping --icf=safe (BFD ld has no ICF, lld is incompatible with GCC LTO)")
    endif()
endif()

# Platform system libraries: matches the LIBS list godot's SCons detect.py
# appends for each platform (godot/platform/<plat>/detect.py). The order
# matters on mingw because winsock / crypto interdepend.
if(WIN32)
    # mingw-w64 system libs, pruned to what zym actually still references
    # after the platform/windows strip. Audited via cross-archive
    # `nm --undefined-only` against every libgodot*.a; only libs with >=1
    # remaining undefined symbol are kept.
    #
    # Extras over the original SCons LIBS list:
    #   - `hid` for `HidD_*` / `HidP_*` (controller probing in os_windows.cpp)
    #
    # zym-dropped, with rationale:
    #   - `gdi32`     : no GDI calls (DisplayServer/window/cursor/font GDI paths gone).
    #   - `shlwapi`   : no Path*/Str*I helpers referenced.
    #   - `shcore`    : no `GetDpiFor*` / `SetProcessDpiAware` (DisplayServer gone).
    #   - `dwmapi`    : no `Dwm*` (DWM compositor calls were DisplayServer-only).
    #   - `imm32`     : no IME hooks (input gone with DisplayServer).
    #   - `winmm`     : no `timeBeginPeriod` / `waveOut*` / midi (audio gone).
    #   - `wsock32`   : redundant with `ws2_32`; only WSARecvEx lives there
    #                   and we don't call it.
    #   - `ole32`/`oleaut32` : `CoInitializeEx` + WMI video-adapter probe gone.
    #   - `dwrite`    : DWrite system-font block was never actually compiled
    #                   in (system-fonts are not stripped at the .cpp level
    #                   yet, but no DWrite symbols leak past the archive
    #                   boundary -- the import lib was unnecessary already).
    set(ZYM_PLATFORM_SYSLIBS
            mingw32
            ws2_32
            iphlpapi
            kernel32 user32 advapi32
            shell32
            bcrypt crypt32
            uuid
            hid
            ntdll  # NtQueryInformationFile in drivers/windows/dir_access_windows.cpp
    )
else()
    set(ZYM_PLATFORM_SYSLIBS pthread dl m)
endif()
# On Windows the platform symbols we actually need (`OS_Windows::*`,
# `FileAccessWindows`, the IAT hooks, etc.) live inside the top-level
# `libgodot.windows.<suffix>.a`, NOT inside `libplatform.windows.<suffix>.a`
# (which only carries the cross-platform `api.o` stubs for android/ios/web/
# visionos and `register_platform_apis.gen.o`). The catch is that the same
# archive also contains `libgodot_windows.o`, which references the real
# `Main::setup` / `Main::cleanup` -- zym replaces both with its own boot
# (src/godot_host.cpp + src/boot/register_core.cpp) and never links
# `main/main.cpp`, so `--whole-archive` over the unfiltered libgodot leaves
# those refs undefined. To avoid editing `godot/` we rebuild a filtered copy
# of `libgodot.windows.a` at configure time with `libgodot_windows.o`
# extracted, then link the filtered archive instead.
#
# Linux doesn't need any of this: SCons there doesn't emit the top-level
# `libgodot.linuxbsd.a` at all (the corresponding `libgodot_linuxbsd.o`
# lives elsewhere or is suppressed), so the original Linux link line just
# names a missing path and BFD ld silently no-ops on it. Keep that
# behaviour by gating purely on `WIN32`.
if(WIN32)
    set(ZYM_GODOT_FILTERED_LIB "${CMAKE_BINARY_DIR}/libgodot_filtered.a")
    set(ZYM_GODOT_FILTERED_DIR "${CMAKE_BINARY_DIR}/godot_filtered_objs")
    # SCons emits `libgodot.windows.a` as a *thin* archive (just paths into
    # `bin/obj/...`, no embedded bytes), so `ar x` refuses it. Instead, ask
    # `ar t` for the member list, drop `libgodot_windows.o`, and build a new
    # thin archive (`ar qcS`) from the remaining paths. The actual work
    # lives in a generated shell script so we don't have to fight ninja's
    # double-`$$` quoting rules for `$()` / `$VAR`.
    set(ZYM_GODOT_FILTER_SCRIPT "${CMAKE_BINARY_DIR}/zym_filter_libgodot.sh")
    file(WRITE "${ZYM_GODOT_FILTER_SCRIPT}"
"#!/usr/bin/env bash
set -euo pipefail
AR='${CMAKE_AR}'
RANLIB='${CMAKE_RANLIB}'
IN='${ZYM_GODOT_LIB_PATH}'
OUT='${ZYM_GODOT_FILTERED_LIB}'
rm -f \"\$OUT\"
mapfile -t members < <(\"\$AR\" t \"\$IN\" | grep -v '/libgodot_windows\\.[^/]*\\.o$')
\"\$AR\" qcS \"\$OUT\" \"\${members[@]}\"
\"\$RANLIB\" \"\$OUT\" 2>/dev/null || true
")
    add_custom_command(
            OUTPUT "${ZYM_GODOT_FILTERED_LIB}"
            COMMAND bash "${ZYM_GODOT_FILTER_SCRIPT}"
            DEPENDS "${ZYM_GODOT_LIB_PATH}" "${ZYM_GODOT_FILTER_SCRIPT}"
            COMMENT "Rebuilding libgodot.windows.a (thin) without libgodot_windows.o (Main::setup/cleanup entry-point)"
            VERBATIM
    )
    add_custom_target(zym_godot_filtered DEPENDS "${ZYM_GODOT_FILTERED_LIB}")
    add_dependencies(zym zym_godot_filtered)
    set(ZYM_GODOT_TOP_ARCHIVE "${ZYM_GODOT_FILTERED_LIB}")
else()
    set(ZYM_GODOT_TOP_ARCHIVE "${ZYM_GODOT_LIB_PATH}")
endif()
target_link_libraries(zym PRIVATE
        #-Wl,--whole-archive
        "${ZYM_GODOT_TOP_ARCHIVE}"
        ${ZYM_GODOT_SUBLIBS}
        #-Wl,--no-whole-archive
        ${ZYM_PLATFORM_SYSLIBS}
)

# Post-build compression via UPX. Runs for Release and Tiny builds, gated on
# USE_COMPRESSION, auto-skipped if `upx` is not available on the host.
# Debug stays unpacked so symbols / sanitizers remain usable.
if(USE_COMPRESSION AND (CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "Tiny"))
    find_program(ZYM_UPX_EXECUTABLE upx)
    if(ZYM_UPX_EXECUTABLE)
        message(STATUS ">> UPX: found at ${ZYM_UPX_EXECUTABLE} (will pack ${CMAKE_BUILD_TYPE} zym)")
        add_custom_command(TARGET zym POST_BUILD
                #COMMAND "${ZYM_UPX_EXECUTABLE}" --best --lzma "$<TARGET_FILE:zym>"
                COMMAND "${ZYM_UPX_EXECUTABLE}" --best --nrv2e "$<TARGET_FILE:zym>"
                COMMENT "Compressing $<TARGET_FILE_NAME:zym> with UPX"
                VERBATIM
        )
    else()
        message(STATUS ">> UPX: not found in PATH; skipping post-build compression")
    endif()
endif()
