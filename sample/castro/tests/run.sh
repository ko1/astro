#!/bin/bash
# Run all castro feature tests under tests/, comparing castro stdout
# against gcc-compiled reference output.  Both must produce the same
# stdout AND the same exit code.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
CASTRO="$HERE/../castro"
TMP="$HERE/../tmp/feat"
mkdir -p "$TMP"

pass=0
fail=0
for c in "$HERE"/test_*.c; do
    base=$(basename "$c" .c)
    ref="$TMP/$base.gcc"
    gcc -O0 -o "$ref" "$c"

    expected_out=$("$ref" 2>/dev/null)
    expected_rc=$?
    actual_out=$("$CASTRO" -q --no-compile "$c" 2>/dev/null)
    actual_rc=$?

    if [ "$expected_out" = "$actual_out" ] && [ "$expected_rc" = "$actual_rc" ]; then
        pass=$((pass+1))
        # echo "PASS $base"
    else
        fail=$((fail+1))
        echo "FAIL $base: rc=$actual_rc expected=$expected_rc"
        diff <(printf '%s' "$expected_out") <(printf '%s' "$actual_out") | head -10
    fi
done

# Also exercise compiled / cached paths — they should produce the same
# stdout and exit code as interp mode.
for c in "$HERE"/test_*.c; do
    base=$(basename "$c" .c)
    ref="$TMP/$base.gcc"
    expected_out=$("$ref" 2>/dev/null)
    expected_rc=$?

    rm -rf "$HERE/../code_store"
    actual_out=$("$CASTRO" -q --compile-all "$c" 2>/dev/null)
    actual_rc=$?
    if [ "$expected_out" = "$actual_out" ] && [ "$expected_rc" = "$actual_rc" ]; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        echo "FAIL (compile-all) $base: rc=$actual_rc expected=$expected_rc"
    fi

    actual_out=$("$CASTRO" -q "$c" 2>/dev/null)
    actual_rc=$?
    if [ "$expected_out" = "$actual_out" ] && [ "$expected_rc" = "$actual_rc" ]; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        echo "FAIL (cached) $base: rc=$actual_rc expected=$expected_rc"
    fi
done

echo "=== feature tests: $pass passed, $fail failed ==="
[ "$fail" = "0" ]
