#!/usr/bin/env bash
# Runs every *.zym test in this folder sequentially, streaming each test's
# stdout/stderr straight through so you can eyeball the results.
#
# Usage:
#   tests/cli/runAll.sh [path/to/zym]
#
# If no binary is given, defaults to cmake-build-linux/zym relative to the
# repo root. Override with the first argument or the ZYM env var.

set -u

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$here/../.." && pwd)"

zym_bin="${1:-${ZYM:-$repo_root/cmake-build-linux/zym}}"

if [ ! -x "$zym_bin" ]; then
    echo "runAll.sh: zym binary not found or not executable: $zym_bin" >&2
    echo "           pass a path as the first argument or set \$ZYM." >&2
    exit 1
fi

shopt -s nullglob
tests=( "$here"/*.zym )
shopt -u nullglob

if [ "${#tests[@]}" -eq 0 ]; then
    echo "runAll.sh: no *.zym tests found in $here" >&2
    exit 1
fi

total=0
failed=0

for t in "${tests[@]}"; do
    total=$((total + 1))
    name="$(basename -- "$t")"
    echo "===== $name ====="
    "$zym_bin" "$t"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        failed=$((failed + 1))
        echo "----- $name exited with status $rc -----"
    fi
    echo
done

echo "===== summary ====="
echo "ran:    $total"
echo "failed: $failed"

[ "$failed" -eq 0 ]
