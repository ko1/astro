#!/usr/bin/env bash
# Optcarrot benchmark across runtimes.  Run from sample/koruby.
#
# Each row reports the program's reported FPS plus the wall-clock total
# (so AOT-first folds in compile cost).
#
# Tunables (env):
#   BENCH_FRAMES=<n>   frames to render per run.  Default 180 — koruby
#                      currently SEGVs in Boehm GC at higher counts.
#   BENCH_RUNS=<n>     repeat each target N times, report best FPS
#                      and corresponding total.  Default 3.

set -uo pipefail

BENCH=${BENCH:-../abruby/benchmark/optcarrot/bin/optcarrot-bench}
RUBY=${RUBY:-ruby}
FRAMES=${BENCH_FRAMES:-180}
RUNS=${BENCH_RUNS:-3}

# Common args: headless, print fps once at the end, fixed frame count.
BENCH_ARGS=(--headless --print-fps --no-print-video-checksum --frames "$FRAMES")

# run_one LABEL RUNS -- CMD ARGS...
#   LABEL  : column header
#   RUNS   : how many times to repeat (best-FPS wins, total comes from
#            that same run).  Use 1 for cold-only measurements like
#            koruby AOT-first.
run_one() {
    local label="$1" runs="$2"; shift 2
    local best_fps="" best_total=""
    for ((i = 0; i < runs; i++)); do
        local t0 t1 elapsed fps output exit_code
        t0=$(date +%s.%N)
        output=$( "$@" "$BENCH" "${BENCH_ARGS[@]}" 2>/dev/null )
        exit_code=$?
        t1=$(date +%s.%N)
        elapsed=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.2f", b - a }')
        if [[ $exit_code -ne 0 ]]; then
            printf "  %-26s %12s %12s\n" "$label" "ERR($exit_code)" "$elapsed"
            return
        fi
        fps=$(echo "$output" | awk -F': *' '/^fps:/ { printf "%.2f", $2; exit }')
        if [[ -z "$best_fps" ]] || \
           awk -v a="$fps" -v b="$best_fps" 'BEGIN { exit !(a > b) }'; then
            best_fps="$fps"
            best_total="$elapsed"
        fi
    done
    printf "  %-26s %12s %12s\n" "$label" "${best_fps:--}" "${best_total:--}"
}

printf "  optcarrot %d frames, best of %d run(s)\n" "$FRAMES" "$RUNS"
printf "  %-26s %12s %12s\n" "TARGET" "FPS" "TOTAL[s]"
printf "  %-26s %12s %12s\n" "------" "---" "--------"

run_one "ruby"             "$RUNS" $RUBY
run_one "ruby --yjit"      "$RUNS" $RUBY --yjit
run_one "koruby (interp)"  "$RUNS" ./koruby

# AOT-first: cold compile + run, single shot — repeating would use
# the cache from the prior run.  Wall time includes make+link.
rm -rf code_store
run_one "koruby AOT-first"  1       ./koruby --aot-compile
# AOT-cached: subsequent runs pick up code_store/all.so via dlopen.
run_one "koruby AOT-cached" "$RUNS" ./koruby
