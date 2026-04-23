set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Tiny")

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer -fno-exceptions")
set(CMAKE_C_FLAGS_DEBUG   "-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-fsanitize=address,undefined")

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG -fno-exceptions -fvisibility=hidden -fvisibility-inlines-hidden -ffunction-sections -fdata-sections")
set(CMAKE_C_FLAGS_RELEASE   "-O3 -march=native -DNDEBUG -fvisibility=hidden -ffunction-sections -fdata-sections")
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

set(CMAKE_BUILD_TYPE Debug)

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
)

set(ZYM_NATIVE_SOURCES
        src/natives/natives.hpp
        src/natives/natives.cpp
        src/natives/print.cpp
        src/natives/time.cpp
        src/natives/buffer.cpp
)

add_executable(zym ${APP_SOURCES} ${ZYM_NATIVE_SOURCES})
if(ZYM_LTO_OK)
    set_property(TARGET zym PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET zym PROPERTY INTERPROCEDURAL_OPTIMIZATION_TINY TRUE)
endif()

target_link_libraries(zym PRIVATE zym_core)

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
        "${ZYM_GODOT_BIN_DIR}/obj/main/libmain.${ZYM_GODOT_LIB_SUFFIX}.a"
        "${ZYM_GODOT_BIN_DIR}/obj/scene/libscene.${ZYM_GODOT_LIB_SUFFIX}.a"
        "${ZYM_GODOT_BIN_DIR}/obj/servers/libservers.${ZYM_GODOT_LIB_SUFFIX}.a"
        "${ZYM_GODOT_BIN_DIR}/obj/drivers/libdrivers.${ZYM_GODOT_LIB_SUFFIX}.a"
        "${ZYM_GODOT_BIN_DIR}/obj/modules/libmodules.${ZYM_GODOT_LIB_SUFFIX}.a"
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
)
# Must mirror the SCons CPPDEFINES that built libgodot.a or the header
# virtual surface drifts and overrides fail to match. See godot/SConstruct.
target_compile_definitions(zym PRIVATE
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

target_link_libraries(zym PRIVATE
        -Wl,--whole-archive
        "${ZYM_GODOT_LIB_PATH}"
        ${ZYM_GODOT_SUBLIBS}
        -Wl,--no-whole-archive
        pthread dl m
)
