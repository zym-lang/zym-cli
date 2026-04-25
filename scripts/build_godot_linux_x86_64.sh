#!/usr/bin/env bash
# Build Godot as a static library for linux_x86_64.
#
# Produces:
#   <repo>/godot/bin/libgodot.linuxbsd.template_release.x86_64.a
#
# Usage:
#   scripts/build_godot_linux_x86_64.sh [extra scons args...]
#
# This script is invoked by the top-level CMakeLists.txt when
# ZYM_PRECOMPILE_GODOT=ON and ZYM_GODOT_PLATFORM=linux_x86_64.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." &>/dev/null && pwd)"
GODOT_DIR="${REPO_ROOT}/godot"

if [[ ! -d "${GODOT_DIR}" ]]; then
    echo "error: godot submodule not found at ${GODOT_DIR}" >&2
    exit 1
fi

if ! command -v scons >/dev/null 2>&1; then
    echo "error: scons is required to build godot but was not found in PATH" >&2
    exit 1
fi

cd "${GODOT_DIR}"

# ClassDB-registration neutering (size lever).
# zym never resolves classes/methods/properties by name through ClassDB; it
# only calls Godot via direct C++ symbols. We therefore strip every
# GDREGISTER_*() expansion from the engine *without modifying godot/* by
# prepending an include-path shim that intercepts the engine header:
#
#   scripts/zym_shim/core/object/class_db.h
#
# is searched before godot/core/object/class_db.h. The shim does
# `#include_next "core/object/class_db.h"` to pull in the real engine
# header, then `#undef`s and redefines the GDREGISTER_* macros to no-ops
# for the rest of the TU. Net: every _bind_methods / MethodBind /
# property / signal / enum table the engine would have emitted is gone.
#
# We deliberately do NOT use `-include` to force-inject class_db.h into
# every TU: some Godot TUs (e.g. core/variant/array.cpp,
# core/variant/dictionary.cpp) compile against intentionally-incomplete
# `Array`/`Dictionary`/`Object`/`String` types and assert that via
# STATIC_ASSERT_INCOMPLETE_TYPE. A force-include of class_db.h transitively
# defines those types and breaks the assertion. The shim approach only
# triggers for TUs that *already* include class_db.h, so the contract is
# preserved.
#
# C TUs are unaffected: they don't include class_db.h, so the shim is never
# resolved and `-I` is harmless.
ZYM_SHIM_DIR="${SCRIPT_DIR}/zym_shim"
ZYM_CCFLAGS="-fvisibility=hidden -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables"
ZYM_CXXFLAGS="-fvisibility-inlines-hidden -I${ZYM_SHIM_DIR} -fno-asynchronous-unwind-tables -fno-unwind-tables"

exec scons \
    platform=linuxbsd \
    target=template_release \
    arch=x86_64 \
    library_type=static_library \
    lto=full \
    optimize=size_extra \
    production=yes \
    \
    modules_enabled_by_default=no \
    \
    vulkan=no \
    opengl3=no \
    x11=no \
    wayland=no \
    libdecor=no \
    touch=no \
    alsa=no \
    pulseaudio=no \
    dbus=no \
    speechd=no \
    fontconfig=no \
    udev=no \
    sdl=no \
    accesskit=no \
    \
    disable_3d=yes \
    disable_advanced_gui=yes \
    disable_xr=yes \
    disable_physics_2d=yes \
    disable_physics_3d=yes \
    disable_navigation_2d=yes \
    disable_navigation_3d=yes \
    \
    debug_symbols=no \
    dev_build=no \
    tests=no \
    deprecated=no \
    minizip=no \
    brotli=no \
    disable_exceptions=yes \
    build_profile="../scripts/zym_profile.gdbuild" \
    ccflags="${ZYM_CCFLAGS}" \
    cxxflags="${ZYM_CXXFLAGS}" \
    "$@"
