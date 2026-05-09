#!/bin/bash
# Compare asml (interp + AOT-warm) against Standard ML of New Jersey (`sml`).
# Run from sample/asml/.  Prints a table of best-of-3 wall times.
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

have_sml=0
if command -v sml >/dev/null 2>&1; then have_sml=1; fi

if [ $have_sml -eq 1 ]; then
    printf "%-12s %10s %10s %10s\n" "bench" "asml-int" "asml-AOT" "sml/NJ"
    printf "%-12s %10s %10s %10s\n" "-----" "--------" "--------" "------"
else
    printf "%-12s %10s %10s\n" "bench" "asml-int" "asml-AOT"
    printf "%-12s %10s %10s\n" "-----" "--------" "--------"
fi

for f in "${bench_files[@]}"; do
    name=$(basename "$f" .sml)
    interp_t=$(best_of_3 "./asml -q $f")
    rm -rf code_store
    ./asml -q -c "$f" >/dev/null
    aot_t=$(best_of_3 "./asml -q -c $f")
    if [ $have_sml -eq 1 ]; then
        # sml prints banner / autoload chatter; suppress with grep, but
        # that's already "free" since /usr/bin/time only measures the
        # whole sml run.  Stdout goes to /dev/null in the timer.
        sml_t=$(best_of_3 "sml $f")
        printf "%-12s %10s %10s %10s\n" "$name" "$interp_t" "$aot_t" "$sml_t"
    else
        printf "%-12s %10s %10s\n" "$name" "$interp_t" "$aot_t"
    fi
done
