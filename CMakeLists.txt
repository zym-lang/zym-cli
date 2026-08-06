set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Tiny")
# NOTE: per-config compile/link flag setup is intentionally below
# `project()` (further down in this file). The mingw toolchain file
# (cmake/toolchains/mingw64.cmake) is loaded as part of project()'s
# compiler-detection step, so `WIN32` and `CMAKE_SYSTEM_NAME` are not
# reliably populated above `project()`. Doing the flag conditionals
# pre-project() caused the Windows branch to silently take the Linux
# path, which re-added `-fvisibility=hidden` under `-Oz -flto` and
# tripped a mingw-gcc ICE in `binds_to_current_def_p`.

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

set(CMAKE_BUILD_TYPE Release)
set(USE_COMPRESSION ON)
# RUNTIME_ONLY: ON builds a runtime-only binary that skips compiling and
# linking `full_executor` and only takes the `runtime_loader` path in main.
set(RUNTIME_ONLY ON)
# USE_ENGINE_ERRORS: ON keeps Godot's `ERROR: ... at: <fn> (file.cpp:N)` lines
# on stderr (the engine's `ERR_FAIL_*` / `ERR_PRINT` macros). OFF compiles the
# binary so engine-level error and warning output is suppressed at startup
# (zym's own runtime errors are unaffected). See src/boot/register_core.cpp.
set(USE_ENGINE_ERRORS OFF)

cmake_minimum_required(VERSION 4.0)
project(zym_cli C CXX)
include(CheckIPOSupported)
check_ipo_supported(RESULT ZYM_LTO_OK OUTPUT ZYM_LTO_ERR)

# --- Per-config compile/link flags --------------------------------------
# Deferred until after `project()` so `WIN32` / `CMAKE_SYSTEM_NAME` are
# populated (the mingw toolchain is loaded during project()'s
# compiler-detection, not before CMakeLists.txt starts).
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

# mingw-w64 GCC has a known LTO ICE in `binds_to_current_def_p`
# (symtab.cc:2497) on this codebase under `-flto=auto` (parallel
# LTO partitioning). The ICE wanders across GIMPLE passes
# (dse, alias, sink, store-merging, ...) as each is disabled —
# the bug is in symbol resolution itself, not the passes, because
# `-flto=auto` shards the symbol table across worker processes
# and resolution mismatches blow up `binds_to_current_def_p`.
# Forcing `-flto-partition=none` (single partition, no sharding)
# is the clean escape. Linux gcc-14 doesn't trip the bug, so this
# is Windows-only. `Release` escapes because its target_compile/
# link_options use plain `-flto` (single-partition) instead of
# the IPO-property-injected `-flto=auto`.
if(WIN32)
    set(_ZYM_TINY_LTO_WORKAROUND "-flto-partition=none")
else()
    set(_ZYM_TINY_LTO_WORKAROUND "")
endif()

set(CMAKE_CXX_FLAGS_TINY "-Oz -DNDEBUG -ffunction-sections -fdata-sections -fno-exceptions -fvisibility=hidden -fno-asynchronous-unwind-tables -fno-unwind-tables ${_ZYM_TINY_LTO_WORKAROUND}")
set(CMAKE_C_FLAGS_TINY   "-Oz -DNDEBUG -ffunction-sections -fdata-sections -fno-exceptions -fvisibility=hidden -fno-asynchronous-unwind-tables -fno-unwind-tables ${_ZYM_TINY_LTO_WORKAROUND}")
set(CMAKE_EXE_LINKER_FLAGS_TINY "-static-libstdc++ -static-libgcc -Wl,--gc-sections -Wl,-s -Wl,-Map,zym_cli.map ${_ZYM_TINY_LTO_WORKAROUND}")

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# add_compile_definitions(DEBUG_CALL)

add_subdirectory(zym_core)

set(ZYM_FRAMES_MAX 4096 CACHE STRING "Max VM call frames (overrides FRAMES_MAX in zym_core/src/vm.h)")
target_compile_definitions(zym_core PUBLIC FRAMES_MAX=${ZYM_FRAMES_MAX})

set(ZYM_STACK_MAX 262144 CACHE STRING "Max VM value stack size (overrides STACK_MAX in zym_core/src/vm.h)")
target_compile_definitions(zym_core PUBLIC STACK_MAX=${ZYM_STACK_MAX})

set(ZYM_PREEMPT_MAX_ENTRIES 32 CACHE STRING "Preemption table slots per VM (overrides ZYM_PREEMPT_MAX_ENTRIES in zym_core/src/vm.h)")
target_compile_definitions(zym_core PUBLIC ZYM_PREEMPT_MAX_ENTRIES=${ZYM_PREEMPT_MAX_ENTRIES})

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
        src/natives/sqlite.cpp
)

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

# --- SQLite (vendored amalgamation) -------------------------------------
# Vendored under third_party/sqlite as the canonical amalgamation
# (sqlite3.c + sqlite3.h, version 3.50.2 — fetched from sqlite.org).
# Built as a small static lib `zym_sqlite` so SQLite's compile flags
# don't leak into the rest of the zym TUs, and so warnings inside
# sqlite3.c are silenced locally.
#
# Compile-flag selection (size + zym fit):
#   * SQLITE_THREADSAFE=0          — every ZymVM is single-threaded and
#                                    each Database handle stays bound to
#                                    its owning VM. No mutex overhead.
#   * SQLITE_OMIT_LOAD_EXTENSION   — `loadExtension` deferred; trims a
#                                    real chunk of code + the libdl dep.
#   * SQLITE_OMIT_DEPRECATED       — drop legacy v1 APIs we don't bind.
#   * SQLITE_OMIT_SHARED_CACHE     — shared-cache is single-process-only,
#                                    irrelevant here, ~adds size.
#   * SQLITE_OMIT_PROGRESS_CALLBACK — not bound on the zym side.
#   * SQLITE_OMIT_DECLTYPE         — column-decltype reflection not bound.
#   * SQLITE_DEFAULT_MEMSTATUS=0   — skip per-allocation tracking; zym
#                                    natives don't expose memstat anyway.
#   * SQLITE_DEFAULT_WAL_SYNCHRONOUS=1 — match better-sqlite3's default.
#   * SQLITE_LIKE_DOESNT_MATCH_BLOBS — small perf win, no behaviour zym
#                                      scripts depend on.
#   * SQLITE_MAX_EXPR_DEPTH=0      — trim expr-depth bookkeeping.
#   * SQLITE_USE_ALLOCA            — small wins on Linux/x86_64.
#   * SQLITE_ENABLE_DESERIALIZE    — required for sqlite3_serialize /
#                                    sqlite3_deserialize, which power the
#                                    Buffer-backed `SQLite.open(buffer)` /
#                                    `db.serialize()` round-trip.
add_library(zym_sqlite STATIC
        "${CMAKE_SOURCE_DIR}/third_party/sqlite/sqlite3.c"
)
target_include_directories(zym_sqlite PUBLIC
        "${CMAKE_SOURCE_DIR}/third_party/sqlite"
)
target_compile_definitions(zym_sqlite PRIVATE
        SQLITE_THREADSAFE=0
        SQLITE_OMIT_LOAD_EXTENSION
        SQLITE_OMIT_DEPRECATED
        SQLITE_OMIT_SHARED_CACHE
        SQLITE_OMIT_PROGRESS_CALLBACK
        SQLITE_DEFAULT_MEMSTATUS=0
        SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
        SQLITE_LIKE_DOESNT_MATCH_BLOBS
        SQLITE_MAX_EXPR_DEPTH=0
        SQLITE_USE_ALLOCA
        SQLITE_ENABLE_DESERIALIZE
        # Column-metadata APIs (sqlite3_column_table_name /
        # _origin_name / _database_name) and decltype are used by
        # `stmt.columns()` and the `expand` row-shape. They're cheap
        # but not on by default in the amalgamation.
        SQLITE_ENABLE_COLUMN_METADATA
)
# sqlite3.c emits a handful of -Wunused warnings under -Wall and the
# zym TUs are fussier. Silence inside the SQLite TU only.
target_compile_options(zym_sqlite PRIVATE -w)
# Match zym's LTO + -fno-pie posture so the final link is consistent
# with libgodot.a (non-PIC, GCC-LTO).
if(ZYM_LTO_OK)
    set_property(TARGET zym_sqlite PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET zym_sqlite PROPERTY INTERPROCEDURAL_OPTIMIZATION_TINY TRUE)
endif()
target_compile_options(zym_sqlite PRIVATE
        $<IF:$<CONFIG:Release>,-flto,-flto=auto>)
if(NOT WIN32)
    target_compile_options(zym_sqlite PRIVATE -fno-pie)
endif()
target_link_libraries(zym PRIVATE zym_sqlite)
message(STATUS ">> SQLite: vendored amalgamation 3.50.2 linked into zym via zym_sqlite")


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
