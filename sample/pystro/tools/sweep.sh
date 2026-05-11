#!/bin/bash
# pystro CPython sweep — run cpython/Lib/test/test_*.py via pystro,
# classify each result, print a summary.  Runs in parallel.
#
# Usage: ./tools/sweep.sh [-j N] [-p PATTERN] [-l LIMIT]
#   -j N        parallel jobs (default: nproc)
#   -p PATTERN  grep pattern on the test filename
#   -l LIMIT    max tests to run

set -u
JOBS=$(nproc 2>/dev/null || echo 4)
PATTERN=""
LIMIT=0
while getopts "j:p:l:" opt; do
    case "$opt" in
        j) JOBS=$OPTARG ;;
        p) PATTERN=$OPTARG ;;
        l) LIMIT=$OPTARG ;;
        *) echo "usage: $0 [-j N] [-p PATTERN] [-l LIMIT]"; exit 2 ;;
    esac
done

OUT=${OUT:-${TMPDIR:-/tmp}/pystro_sweep}
mkdir -p "$OUT" "$OUT/logs"
: > "$OUT/results.txt"

cd "$(dirname "$0")/.."
[ -x ./pystro ] || { echo "build pystro first"; exit 1; }

classify_one() {
    local f="$1"
    local name
    name=$(basename "$f" .py)
    local log="$OUT/logs/$name.log"
    timeout 30 env PYTHONPATH=cpytest_stubs:cpython/Lib ./pystro -q "$f" >"$log" 2>&1
    local rc=$?

    local kind
    if [ $rc -eq 124 ]; then
        kind="TIMEOUT"
    elif [ $rc -ge 128 ] || grep -q "Segmentation\|Aborted\|core dumped" "$log"; then
        kind="CRASH"
    elif grep -q "^SyntaxError:\|^IndentationError:" "$log"; then
        kind="PARSE_ERR"
    elif grep -q "^ModuleNotFoundError\|^ImportError" "$log"; then
        kind="IMPORT_ERR"
    elif grep -q "FAILED (" "$log" || grep -q "^FAIL " "$log"; then
        kind="MIXED"
    elif grep -qE "^OK\b|^passed=[0-9]+ failed=0" "$log"; then
        kind="PASS"
    else
        kind="OTHER"
    fi
    echo "$kind $name $rc"
}
export -f classify_one
export OUT

mapfile -t TESTS < <(ls cpython/Lib/test/test_*.py)
if [ -n "$PATTERN" ]; then
    TESTS=($(printf '%s\n' "${TESTS[@]}" | grep "$PATTERN"))
fi
if [ "$LIMIT" -gt 0 ]; then
    TESTS=("${TESTS[@]:0:$LIMIT}")
fi

printf '%s\n' "${TESTS[@]}" | xargs -I{} -P "$JOBS" bash -c 'classify_one "$@"' _ {} \
    > "$OUT/results.txt"

echo "--- summary ---"
awk '{print $1}' "$OUT/results.txt" | sort | uniq -c | sort -rn
echo "total=${#TESTS[@]}"
