#!/bin/bash
# bench/cross/bench.sh — Phase 11 cross-Scheme benchmark.
#
# Runs each .scm benchmark across:
#   - ascheme_precise   (plain, AOT-cached)
#   - chez (chezscheme 9.5.8)
#   - guile-3.0
#   - gambit (gsi)
#   - chicken (csi interpreter; csc compiled if available)
#   - racket
#   - chibi-scheme
#
# Picks best-of-3 (= min real time) per scheme/bench pair.  Output is
# a TSV table to stdout.  Pre-condition: code_store/all.so already
# baked by `./ascheme_precise --pg-compile -q <each bench>` (= a separate
# step; bench.sh assumes the cache is current).
#
# Usage: bash bench/cross/bench.sh [bench-files...]

set -u
cd "$(dirname "$0")/../.."   # cwd = sample/ascheme_precise/

BENCHES="${*:-bench/cross/fib35.scm bench/cross/tarai.scm bench/cross/ack.scm bench/cross/sum.scm bench/cross/sieve.scm bench/cross/nqueens.scm}"

TIMEOUT=120  # seconds

run_timed() {
    # $1: label printed in trace
    # $2..: command + args
    local label="$1"; shift
    local t1=$(date +%s.%N)
    if timeout "$TIMEOUT" "$@" >/dev/null 2>&1; then
        local t2=$(date +%s.%N)
        awk -v a="$t1" -v b="$t2" 'BEGIN { printf "%.3f", b - a }'
    else
        echo "TO"
    fi
}

best_of_3() {
    local label="$1"; shift
    local results=()
    for i in 1 2 3; do
        results+=( "$(run_timed "$label" "$@")" )
    done
    # Print min; "TO" if any timeout
    echo "${results[@]}" | tr ' ' '\n' | sort -n | head -1
}

# Header
printf "%-30s" "benchmark"
printf "%-12s" "precise-AOT" "precise-plain" "chez" "racket" "gambit-gsi" "chibi" "guile-3.0" "chicken-csi"
echo

# Body
for bench in $BENCHES; do
    name=$(basename "$bench" .scm)
    racket_bench="bench/cross/racket/${name}.scm"
    chibi_bench="bench/cross/chibi/${name}.scm"
    printf "%-30s" "$name"

    # ascheme_precise AOT (= no --plain, cache auto-loads)
    printf "%-12s" "$(best_of_3 prec-aot ./ascheme_precise -q "$bench")"

    # ascheme_precise plain
    printf "%-12s" "$(best_of_3 prec-plain ./ascheme_precise --plain -q "$bench")"

    # chez (chezscheme 9.5.8)
    printf "%-12s" "$(best_of_3 chez scheme --script "$bench")"

    # racket (uses #lang racket/base wrapper)
    if [ -f "$racket_bench" ]; then
        printf "%-12s" "$(best_of_3 racket racket "$racket_bench")"
    else
        printf "%-12s" "NA"
    fi

    # gambit gsi
    printf "%-12s" "$(best_of_3 gambit gsi "$bench")"

    # chibi-scheme (uses r7rs (import ...) wrapper)
    if [ -f "$chibi_bench" ]; then
        printf "%-12s" "$(best_of_3 chibi chibi-scheme "$chibi_bench")"
    else
        printf "%-12s" "NA"
    fi

    # guile-3.0
    printf "%-12s" "$(best_of_3 guile guile-3.0 -s "$bench")"

    # chicken csi (interpreter)
    printf "%-12s" "$(best_of_3 chicken csi -s "$bench")"

    echo
done
