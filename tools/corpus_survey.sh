#!/usr/bin/env bash
# Compatibility survey: run `kamipy build` (and the produced binary) over a
# corpus of real-world Python files and classify the failures.
#
# Usage:
#   tools/corpus_survey.sh <path-to-kamipy> <corpus-dir> <output-dir>
#
# Example corpus:
#   git clone --depth 1 https://github.com/TheAlgorithms/Python /tmp/corpus/algorithms
#   git clone --depth 1 https://github.com/geekcomputers/Python /tmp/corpus/geekcomputers
set -u
KAMIPY="$1"; CORPUS="$2"; OUT="$3"
mkdir -p "$OUT"
: > "$OUT/errors.txt"; : > "$OUT/built.txt"; : > "$OUT/ran.txt"; : > "$OUT/run_fail.txt"
find "$CORPUS" -name "*.py" | sort | while read -r f; do
    err=$("$KAMIPY" build "$f" -o /tmp/kamipy_survey_bin 2>&1 >/dev/null)
    if [ $? -eq 0 ]; then
        echo "$f" >> "$OUT/built.txt"
        if timeout 5 /tmp/kamipy_survey_bin </dev/null >/dev/null 2>/tmp/kamipy_survey_err; then
            echo "$f" >> "$OUT/ran.txt"
        else
            echo "$f | $(head -c 200 /tmp/kamipy_survey_err | tr '\n' ' ')" >> "$OUT/run_fail.txt"
        fi
    else
        msg=$(echo "$err" | head -1 | sed 's/^[^:]*:[0-9]*: error: //' | sed "s/'[^']*'/'X'/g" | head -c 120)
        echo "$msg | $f" >> "$OUT/errors.txt"
    fi
done
echo "total: $(find "$CORPUS" -name '*.py' | wc -l)"
echo "built: $(wc -l < "$OUT/built.txt")"
echo "ran:   $(wc -l < "$OUT/ran.txt")"
echo
echo "top error classes:"
cut -d'|' -f1 "$OUT/errors.txt" | sort | uniq -c | sort -rn | head -20
