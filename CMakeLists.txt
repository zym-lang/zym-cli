set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Tiny")

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer -fno-exceptions")
set(CMAKE_C_FLAGS_DEBUG   "-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-fsanitize=address,undefined")

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -fno-exceptions -fvisibility=hidden -fvisibility-inlines-hidden -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables")
set(CMAKE_C_FLAGS_RELEASE   "-O3 -DNDEBUG -fvisibility=hidden -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables")
#set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-static-libstdc++ -static-libgcc -Wl,--gc-sections -Wl,-s")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-static-libstdc++ -static-libgcc -Wl,--gc-sections")

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

set(CMAKE_BUILD_TYPE Release)
set(USE_COMPRESSION ON)
# USE_ENGINE_ERRORS: ON keeps Godot's `ERROR: ... at: <fn> (file.cpp:N)` lines
# on stderr (the engine's `ERR_FAIL_*` / `ERR_PRINT` macros). OFF compiles the
# binary so engine-level error and warning output is suppressed at startup
# (zym's own runtime errors are unaffected). See src/boot/register_core.cpp.
set(USE_ENGINE_ERRORS OFF)

cmake_minimum_required(VERSION 4.0)
project(zym_cli C CXX)
include(CheckIPOSupported)
check_ipo_supported(RESULT ZYM_LTO_OK OUTPUT ZYM_LTO_ERR)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# add_compile_definitions(DEBUG_CALL)

add_subdirectory(zym_core)

set(APP_SOURCES
        src/main.cpp
        src/runtime_loader.hpp
        src/runtime_loader.cpp
        src/full_executor.hpp
        src/full_executor.cpp
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

set(ZYM_NATIVE_SOURCES
        src/natives/natives.hpp
        src/natives/natives.cpp
        src/natives/cli_catalog.hpp
        src/natives/cli_catalog.cpp
        src/natives/Zym/zym_native.hpp
        src/natives/Zym/zym_native.cpp
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
        src/natives/aes.cpp
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
set(ZYM_GODOT_PLATFORM   "linux_x86_64")

set(ZYM_GODOT_DIR     "${CMAKE_SOURCE_DIR}/godot")
set(ZYM_GODOT_BIN_DIR "${ZYM_GODOT_DIR}/bin")

if(ZYM_GODOT_PLATFORM STREQUAL "linux_x86_64")
    set(ZYM_GODOT_LIB_FILE   "libgodot.linuxbsd.template_release.x86_64.a")
    set(ZYM_GODOT_BUILD_SH   "${CMAKE_SOURCE_DIR}/scripts/build_godot_linux_x86_64.sh")
else()
    message(FATAL_ERROR "ZYM_GODOT_PLATFORM='${ZYM_GODOT_PLATFORM}' is not supported yet.")
endif()

set(ZYM_GODOT_LIB_PATH "${ZYM_GODOT_BIN_DIR}/${ZYM_GODOT_LIB_FILE}")

# libgodot.<suffix>.a is just the platform entry-point; real symbols live in
# per-subsystem archives under godot/bin/obj/<subsystem>/. Link them all
# (core last so forward-refs resolve).
if(ZYM_GODOT_PLATFORM STREQUAL "linux_x86_64")
    set(ZYM_GODOT_LIB_SUFFIX "linuxbsd.template_release.x86_64")
endif()
set(ZYM_GODOT_SUBLIBS
        "${ZYM_GODOT_BIN_DIR}/obj/platform/libplatform.${ZYM_GODOT_LIB_SUFFIX}.a"
        "${ZYM_GODOT_BIN_DIR}/obj/drivers/libdrivers.${ZYM_GODOT_LIB_SUFFIX}.a"
        "${ZYM_GODOT_BIN_DIR}/obj/modules/libmodules.${ZYM_GODOT_LIB_SUFFIX}.a"
        "${ZYM_GODOT_BIN_DIR}/obj/modules/libmodule_regex.${ZYM_GODOT_LIB_SUFFIX}.a"
        "${ZYM_GODOT_BIN_DIR}/obj/modules/libmodule_mbedtls.${ZYM_GODOT_LIB_SUFFIX}.a"
        "${ZYM_GODOT_BIN_DIR}/obj/modules/libmodule_enet.${ZYM_GODOT_LIB_SUFFIX}.a"
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
target_include_directories(zym PRIVATE
        "${ZYM_GODOT_DIR}"
        "${ZYM_GODOT_DIR}/platform/linuxbsd"
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
        # From godot/platform/linuxbsd/detect.py (unconditional for linuxbsd):
        LINUXBSD_ENABLED
        UNIX_ENABLED
        _FILE_OFFSET_BITS=64
)
# -flto: required because libgodot.a was built with `lto=full` (stores GCC
# LTO IR, not native objects). Full (single-partition) in Release for max
# cross-module inlining/DCE; auto (parallel) elsewhere for faster links.
# -fno-pie/-no-pie: libgodot.a is non-PIC (static_library omits -fPIC); match
# it to avoid DT_TEXTREL in a PIE. Remove if Godot is rebuilt with -fPIC.
target_compile_options(zym PRIVATE
        $<IF:$<CONFIG:Release>,-flto,-flto=auto>
        -fno-pie)
target_link_options(zym PRIVATE
        $<IF:$<CONFIG:Release>,-flto,-flto=auto>
        -no-pie)

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

target_link_libraries(zym PRIVATE
        -Wl,--whole-archive
        "${ZYM_GODOT_LIB_PATH}"
        ${ZYM_GODOT_SUBLIBS}
        -Wl,--no-whole-archive
        pthread dl m
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
