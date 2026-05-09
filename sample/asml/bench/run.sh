#!/bin/bash
# Run each bench in: (1) interpreter, (2) AOT cold (compile + run), (3) AOT warm
# (compile cached).  Reports wall-time best-of-3.  Run from sample/asml/.
set -eu
cd "$(dirname "$0")/.."

bench_files=(bench/fib.sml bench/ack.sml bench/tak.sml bench/nqueens.sml)

best_of_3() {
    local cmd="$1"
    local best=999.999
    for _ in 1 2 3; do
        local t
        t=$( { /usr/bin/time -f '%e' bash -c "$cmd" >/dev/null; } 2>&1 )
        if awk "BEGIN{exit !($t < $best)}"; then best=$t; fi
    done
    echo "$best"
}

printf "%-12s %10s %10s %10s\n" "bench" "interp" "AOT-cold" "AOT-warm"
printf "%-12s %10s %10s %10s\n" "-----" "------" "--------" "--------"
for f in "${bench_files[@]}"; do
    name=$(basename "$f" .sml)

    interp_t=$(best_of_3 "./asml -q $f")

    rm -rf code_store
    cold_t=$(best_of_3 "rm -rf code_store; ./asml -q -c $f")

    # warm: code_store already populated from above
    rm -rf code_store
    ./asml -q -c "$f" >/dev/null
    warm_t=$(best_of_3 "./asml -q -c $f")

    printf "%-12s %10s %10s %10s\n" "$name" "$interp_t" "$cold_t" "$warm_t"
done
