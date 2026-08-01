#!/usr/bin/env bash
# Run a subset of the integration tests with the GC collecting on every
# allocation: catches values that are not reachable from a registered root.
set -u
KAMIPY="$1"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0
for base in hello arith strings lists dicts functions feat_classes feat_closures \
            feat_exceptions feat_comprehensions feat_builtins feat_unpacking \
            feat_dict_order feat_module_ns feat_stdlib feat_regex gc_stress \
            proj_quicksort proj_text_analytics feat_files_sets; do
    py="$DIR/$base.py"
    [ -f "$py" ] || continue
    if ! "$KAMIPY" build "$py" -o "$TMP/$base" > "$TMP/log" 2>&1; then
        echo "BUILD FAIL: $base"; cat "$TMP/log"; fail=1; continue
    fi
    if ! out="$(KAMIPY_GC_STRESS=1 timeout 600 "$TMP/$base" 2>"$TMP/err")"; then
        echo "GC-STRESS FAIL: $base (exit $?)"; cat "$TMP/err"; fail=1; continue
    fi
    if [ "$out" != "$(cat "$DIR/$base.expected")" ]; then
        echo "GC-STRESS MISMATCH: $base"; fail=1
    else
        echo "PASS(gc-stress): $base"
    fi
done
exit $fail
