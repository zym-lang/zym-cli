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

exec scons \
    platform=linuxbsd \
    target=template_release \
    arch=x86_64 \
    library_type=static_library \
    lto=full \
    optimize=speed \
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
    use_static_cpp=yes \
    disable_exceptions=yes \
    "$@"
