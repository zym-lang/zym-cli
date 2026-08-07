#!/usr/bin/env bash
# Runs every *.zym test in this folder sequentially, streaming each test's
# stdout/stderr straight through so you can eyeball the results.
#
# Usage:
#   tests/core/runAll.sh [path/to/zym]
#
# If no binary is given, defaults to cmake-build-linux/zym relative to the
# repo root. Override with the first argument or the ZYM env var.
#
# ---------------------------------------------------------------------------
# HOW A TEST IS JUDGED
#
# The exit code is NOT the signal. The VM deliberately does not take its host
# down over a script error -- that is the whole point of the sandbox -- so the
# CLI returns 0 whether a script completed or died on line one. A runner that
# only checks `rc` therefore cannot tell a passing file from a broken one, and
# for a while this one could not: a compiler regression left tco_variadic.zym
# erroring a third of the way through and this script reported "failed: 0".
#
# So a file fails if ANY of these hold:
#
#   1. it exits non-zero          -- crash, timeout, or the harness itself
#   2. it writes to stderr        -- an uncaught runtime or compile error;
#                                    script output goes to stdout, errors do not
#   3. stdout reports failures    -- "FAILED:", "Tests Failed: n", "Failed: n"
#
# A test that means to provoke an error (error paths are worth covering) opts
# out of rule 2 with a marker anywhere in the file:
#
#   // @expect-stderr   this test deliberately triggers an error
#
# Prefer the native `assert(cond, msg)` in new tests: it raises, so a failure
# stops the file and trips rule 2 on its own. The older hand-rolled
# `assert_wrap` helpers only print and count, which rule 3 covers.
# ---------------------------------------------------------------------------

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
failed_names=()

err_file="$(mktemp)"
trap 'rm -f "$err_file"' EXIT

for t in "${tests[@]}"; do
    total=$((total + 1))
    name="$(basename -- "$t")"
    echo "===== $name ====="

    # stdout streams through as before; stderr is teed so it stays visible
    # while still being inspectable.
    out="$("$zym_bin" "$t" 2>"$err_file")"
    rc=$?
    err="$(cat "$err_file")"

    printf '%s\n' "$out"
    [ -n "$err" ] && printf '%s\n' "$err" >&2

    reason=""

    if [ "$rc" -ne 0 ]; then
        reason="exited with status $rc"
    elif [ -n "$err" ] && ! grep -q '@expect-stderr' "$t"; then
        reason="wrote to stderr (uncaught error); add '// @expect-stderr' if deliberate"
    elif printf '%s' "$out" | grep -qE '^FAILED:|Tests Failed: *[1-9]|Failed: *[1-9]'; then
        reason="reported assertion failures"
    fi

    if [ -n "$reason" ]; then
        failed=$((failed + 1))
        failed_names+=("$name: $reason")
        echo "----- FAIL $name -- $reason -----"
    fi
    echo
done

echo "===== summary ====="
echo "ran:    $total"
echo "failed: $failed"
for f in "${failed_names[@]:-}"; do
    [ -n "$f" ] && echo "  - $f"
done

[ "$failed" -eq 0 ]
