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
    # Each child pystro gets its own TMPDIR so concurrent jobs don't
    # collide on TESTFN (which is `tempfile.gettempdir() +
    # "/@test_pystro_<pid>"`).  Inside the sandbox the spawned pystro
    # always starts at PID ~1, so without a per-job dir every parallel
    # job races for the same /tmp/@test_pystro_1 path.
    local jobtmp
    jobtmp=$(mktemp -d "${TMPDIR:-/tmp}/pystro_sweep_$name.XXXXXX")
    local repo_root="$PWD"
    # Run with cwd = jobtmp so relative TESTFN values don't clash with
    # parallel jobs.  PYTHONPATH stays absolute via $repo_root.
    timeout 60 env -C "$jobtmp" \
        PYTHONPATH="$repo_root/cpytest_stubs:$repo_root/cpython/Lib" \
        TMPDIR="$jobtmp" "$repo_root/pystro" -q "$repo_root/$f" >"$log" 2>&1
    local rc=$?
    rm -rf "$jobtmp" 2>/dev/null

    local kind
    if [ $rc -eq 124 ] || grep -q "^timeout: the monitored command" "$log"; then
        kind="TIMEOUT"
    elif [ $rc -ge 128 ] || grep -q "Segmentation\|Aborted" "$log"; then
        kind="CRASH"
    elif grep -q "^SyntaxError:\|^IndentationError:" "$log"; then
        kind="PARSE_ERR"
    elif grep -q "^ModuleNotFoundError\|^ImportError" "$log"; then
        kind="IMPORT_ERR"
    elif grep -q "FAILED (" "$log" || grep -q "^FAIL " "$log"; then
        kind="MIXED"
    elif grep -qE "^OK\b|^passed=[0-9]+ failed=0" "$log"; then
        kind="PASS"
    elif grep -q "^SkipTest:\|^unittest.SkipTest:\|raise unittest\.SkipTest\b" "$log"; then
        # File-level SkipTest before any unittest runner started:
        # treat as SKIP rather than OTHER (the test would skip on
        # CPython too if the resource were missing).
        kind="SKIP"
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
