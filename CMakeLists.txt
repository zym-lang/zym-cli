set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Tiny")

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer -fno-exceptions -fno-rtti")
set(CMAKE_C_FLAGS_DEBUG   "-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-fsanitize=address,undefined")

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG -fno-exceptions -fno-rtti")
set(CMAKE_C_FLAGS_RELEASE   "-O3 -march=native -DNDEBUG")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-static-libstdc++ -static-libgcc")

set(CMAKE_CXX_FLAGS_TINY "-Oz -DNDEBUG -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -fvisibility=hidden -fno-asynchronous-unwind-tables -fno-unwind-tables")
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
)

set(ZYM_NATIVE_SOURCES
        src/natives/natives.hpp
        src/natives/natives.cpp
        src/natives/print.cpp
        src/natives/clock.cpp
)

add_executable(zym ${APP_SOURCES} ${ZYM_NATIVE_SOURCES})
if(ZYM_LTO_OK)
    set_property(TARGET zym PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET zym PROPERTY INTERPROCEDURAL_OPTIMIZATION_TINY TRUE)
endif()

target_link_libraries(zym PRIVATE zym_core)

# ---------------------------------------------------------------------------
# Godot static library integration
# ---------------------------------------------------------------------------
# ZYM_PRECOMPILE_GODOT : when ON, invoke the platform-specific script to
#                       (re)build libgodot.a before compiling zym. When OFF,
#                       the archive is expected to already exist on disk
#                       (built manually).
# ZYM_GODOT_PLATFORM   : selects which build script / artifact to use.
#                       Currently supported: linux_x86_64.
# Plain (non-cache) variables: the value written here is always the source of
# truth. Editing this file and reconfiguring is the only way to change them -
# no CLion UI toggle, no stale CMakeCache.txt entry can override the setting.
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
    # Make sure generators (notably Ninja) know this path is produced by a
    # build rule even though it is consumed via an IMPORTED target's
    # IMPORTED_LOCATION rather than as a regular source.
    set_source_files_properties("${ZYM_GODOT_LIB_PATH}" PROPERTIES GENERATED TRUE)
else()
    message(STATUS ">> Godot: precompile DISABLED (expecting ${ZYM_GODOT_LIB_PATH})")
    add_custom_target(godot_static_lib)
endif()

add_library(godot_lib STATIC IMPORTED GLOBAL)
set_target_properties(godot_lib PROPERTIES
        IMPORTED_LOCATION "${ZYM_GODOT_LIB_PATH}"
        INTERFACE_INCLUDE_DIRECTORIES "${ZYM_GODOT_DIR}"
)
# Tie the build-rule-producing custom target to the IMPORTED library so any
# consumer of godot_lib transitively picks up the dependency on the archive
# being built first. add_dependencies() on an IMPORTED target requires
# CMake >= 3.11 (we require 4.0).
add_dependencies(godot_lib godot_static_lib)
# Godot core pulls in pthread/dl/m even with everything stripped.
target_link_libraries(godot_lib INTERFACE pthread dl m)

add_dependencies(zym godot_static_lib)
target_link_libraries(zym PRIVATE godot_lib)
