#!/usr/bin/env bash
# Compiles and runs every *.c robustness test in this folder.
#
# These differ from tests/core and tests/cli: they are C programs that drive
# the embedding API with input the compiler would never emit, so they need
# linking against zym_core rather than a zym binary.
#
# Usage:
#   tests/robustness/runAll.sh [iterations] [path/to/libzym_core.a]
#
# Defaults to cmake-build-linux/zym_core/libzym_core.a relative to the repo
# root; override with the second argument or $ZYM_CORE_LIB. `iterations`
# is passed to tests that accept a workload size (the fuzzer), so a longer
# soak is just:
#
#   tests/robustness/runAll.sh 50000
#
# A test fails if it exits non-zero. That deliberately includes dying on a
# signal, so a segfault is a reported failure rather than a silent pass.

set -u

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$here/../.." && pwd)"

iterations="${1:-4000}"
lib="${2:-${ZYM_CORE_LIB:-$repo_root/cmake-build-linux/zym_core/libzym_core.a}}"

if [ ! -f "$lib" ]; then
    echo "runAll.sh: libzym_core.a not found: $lib" >&2
    echo "           build zym_core first, pass a path, or set \$ZYM_CORE_LIB." >&2
    exit 1
fi

# The generated zym/config.h lives in the build tree next to the library.
lib_dir="$(cd -- "$(dirname -- "$lib")" && pwd)"
includes=( -I "$repo_root/zym_core/include" )
if [ -d "$lib_dir/generated" ]; then
    includes+=( -I "$lib_dir/generated" )
fi

cc="${CC:-cc}"
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

shopt -s nullglob
tests=( "$here"/*.c )
shopt -u nullglob

if [ "${#tests[@]}" -eq 0 ]; then
    echo "runAll.sh: no *.c tests found in $here" >&2
    exit 1
fi

total=0
failed=0

for t in "${tests[@]}"; do
    total=$((total + 1))
    name="$(basename -- "$t" .c)"
    echo "===== $name ====="

    if ! "$cc" -O1 -g "${includes[@]}" "$t" "$lib" -lm -o "$workdir/$name" 2>&1; then
        failed=$((failed + 1))
        echo "----- $name failed to compile -----"
        echo
        continue
    fi

    "$workdir/$name" "$iterations"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        failed=$((failed + 1))
        if [ "$rc" -gt 128 ]; then
            echo "----- $name died on signal $((rc - 128)) (crash = failure) -----"
        else
            echo "----- $name exited with status $rc -----"
        fi
    fi
    echo
done

echo "===== summary ====="
echo "ran:    $total"
echo "failed: $failed"

[ "$failed" -eq 0 ]
