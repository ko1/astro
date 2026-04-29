#!/bin/bash
# Run c-testsuite against castro.
# Output: pass/fail/parse_err counts + a categorized failure breakdown.

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TESTS="$HERE/testsuite/c-testsuite/single-exec"
TMP="$HERE/tmp/cts"
CASTRO="$HERE/castro"
PARSE="$HERE/parse.rb"

mkdir -p "$TMP"
rm -f "$TMP"/*.sx "$TMP"/*.out "$TMP"/*.err 2>/dev/null

pass=0
fail_run=0
fail_diff=0
parse_err=0

declare -A reason_count
fail_list=()
parse_err_list=()

for c in "$TESTS"/*.c; do
    base=$(basename "$c" .c)
    expected="$TESTS/$base.c.expected"
    [ -f "$expected" ] || continue

    sx="$TMP/$base.sx"
    perr="$TMP/$base.parse.err"
    if ! ruby "$PARSE" "$c" > "$sx" 2> "$perr"; then
        parse_err=$((parse_err+1))
        reason=$(head -1 "$perr" | sed 's/^.*: //')
        reason_count["parse: $reason"]=$((${reason_count["parse: $reason"]:-0}+1))
        parse_err_list+=("$base: $reason")
        continue
    fi

    out="$TMP/$base.out"
    timeout 10 "$CASTRO" -q --no-compile --sx "$sx" > "$out" 2>/dev/null
    rc=$?
    if [ $rc -ne 0 ]; then
        fail_run=$((fail_run+1))
        reason_count["run: exit $rc"]=$((${reason_count["run: exit $rc"]:-0}+1))
        fail_list+=("$base: rc=$rc")
        continue
    fi

    if diff -q "$out" "$expected" > /dev/null 2>&1; then
        pass=$((pass+1))
    else
        fail_diff=$((fail_diff+1))
        fail_list+=("$base: stdout mismatch")
    fi
done

total=$(ls "$TESTS"/*.c | wc -l)
echo "=== c-testsuite: $pass / $total passed ==="
echo "  parse_err: $parse_err"
echo "  run_fail:  $fail_run"
echo "  diff_fail: $fail_diff"
echo
echo "=== top failure reasons ==="
for k in "${!reason_count[@]}"; do
    printf "  %4d  %s\n" "${reason_count[$k]}" "$k"
done | sort -rn | head -20

if [ "${1:-}" = "-v" ]; then
    echo
    echo "=== parse_err details (first 20) ==="
    printf "%s\n" "${parse_err_list[@]:0:20}"
    echo
    echo "=== fail details (first 20) ==="
    printf "%s\n" "${fail_list[@]:0:20}"
fi
