#!/usr/bin/env bash
# Integration tests: compile every .py with kamipy, run the native binary,
# and compare stdout with the .expected file.
#
# Set KAMIPY_GC_STRESS=1 in the environment to run the same programs with the GC
# collecting before every allocation (see the integration.gc_stress test).
set -u
KAMIPY="$1"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
count=0
for py in "$DIR"/*.py; do
    base="$(basename "$py" .py)"
    exp="$DIR/$base.expected"
    count=$((count + 1))
    # <base>.args (optional): extra inputs for the compiler, e.g. a C source to
    # link in — one per line, resolved relative to this directory.
    extra=()
    if [ -f "$DIR/$base.args" ]; then
        while read -r line; do
            [ -n "$line" ] || continue
            case "$line" in -*) extra+=("$line");; *) extra+=("$DIR/$line");; esac
        done < "$DIR/$base.args"
    fi
    if ! "$KAMIPY" build "$py" "${extra[@]+"${extra[@]}"}" -o "$TMP/$base" \
            > "$TMP/$base.buildlog" 2>&1; then
        echo "BUILD FAIL: $base"
        cat "$TMP/$base.buildlog"
        fail=1
        continue
    fi
    if ! out="$("$TMP/$base" 2>"$TMP/$base.err")"; then
        echo "RUN FAIL: $base (exit $?)"
        cat "$TMP/$base.err"
        fail=1
        continue
    fi
    expected="$(cat "$exp")"
    if [ "$out" != "$expected" ]; then
        echo "OUTPUT MISMATCH: $base"
        echo "--- expected ---"
        echo "$expected"
        echo "--- got ---"
        echo "$out"
        fail=1
    else
        echo "PASS: $base"
    fi
done

echo "ran $count integration tests"
exit $fail
