#!/usr/bin/env bash
# Integration tests: compile every .py with kamipy, run the native binary,
# and compare stdout with the .expected file.
#
# Two groups of tests are conditional:
#   *_posix.py            skipped on Windows (they name libm.so.6 and friends)
#   feat_system_stdlib.py skipped when no Python installation is discoverable,
#                         since it compiles modules out of the machine's stdlib
set -u
KAMIPY="$1"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

case "$(uname -s 2>/dev/null || echo Windows)" in
    MINGW* | MSYS* | CYGWIN* | Windows*) IS_WINDOWS=1 ;;
    *) IS_WINDOWS=0 ;;
esac
if "$KAMIPY" paths 2>/dev/null | grep -q "none found"; then
    HAVE_SYSTEM_PYTHON=0
else
    HAVE_SYSTEM_PYTHON=1
fi

fail=0
count=0
skipped=0
for py in "$DIR"/*.py; do
    base="$(basename "$py" .py)"
    exp="$DIR/$base.expected"
    case "$base" in
        *_posix)
            if [ "$IS_WINDOWS" = 1 ]; then
                echo "SKIP: $base (POSIX only)"
                skipped=$((skipped + 1))
                continue
            fi
            ;;
        feat_system_stdlib)
            if [ "$HAVE_SYSTEM_PYTHON" = 0 ]; then
                echo "SKIP: $base (no Python installation found)"
                skipped=$((skipped + 1))
                continue
            fi
            ;;
    esac
    count=$((count + 1))
    if ! "$KAMIPY" build "$py" -o "$TMP/$base" > "$TMP/$base.buildlog" 2>&1; then
        echo "BUILD FAIL: $base"
        cat "$TMP/$base.buildlog"
        fail=1
        continue
    fi
    if ! out="$(cd "$TMP" && "$TMP/$base" 2>"$TMP/$base.err")"; then
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

echo "ran $count integration tests ($skipped skipped)"
exit $fail
