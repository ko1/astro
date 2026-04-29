#!/bin/bash
# Side-by-side bench comparison: asom (interp/aot/pg) vs other SOM impls.
#
#   ./run_compare.sh [N]            inner-work seconds (default; system ticks)
#   ./run_compare.sh --wall [N]     wall-clock seconds (incl. startup, JVM,
#                                   parser, eager JIT — basically `time`)
#
# Inner-work mode extracts the engine's own `system ticks` measurement of
# the benchmark loop:
#   "<Bench>: iterations=N runtime: Yus"            (asom Bench.som)
#   "<Bench>: iterations=N average: Xus total: Yus" (SOM-st upstream)
# It excludes process startup, .som parse, JVM bootstrap, and eager JIT —
# i.e. is fair across implementations whose startup costs differ wildly
# (CSOM ~700 ms class-load, TruffleSOM ~1.7 s JVM + JIT). Lazy JIT and GC
# pauses inside the loop are included.
#
# Wall mode is `time -f '%e'` over the entire process. Useful for "user
# experience" comparisons but biased against JVM-hosted engines.
#
# Engines:
#   asom/interp : --plain (no code store)
#   asom/aot    : -c --preload=<bench>  (bake before run; subsequent runs
#                                        re-use code_store/all.so)
#   asom/pg     : -p --preload=<bench>  (post-run hot bake; second run uses)
#   SOM++       : SOM-st/SOMpp, USE_TAGGING + COPYING + g++ -O3 -flto
#   Truffle     : SOM-st/TruffleSOM on GraalVM CE 25 with libgraal
#   PySOM-AST   : SOM-st/PySOM AST interpreter, plain CPython 3.12
#   PySOM-BC   : SOM-st/PySOM bytecode interpreter, plain CPython 3.12
#   CSOM        : SOM-st/CSOM, plain C bytecode VM, gcc -O3
#
# All engines run the same SOM/Examples/Benchmarks/<Name>.som from the
# SOM-st/SOM submodule.

set -eu

MODE=inner
if [ "${1:-}" = "--wall" ]; then
    MODE=wall
    shift
fi
ITERS="${1:-50}"

ASOM_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CSOM_DIR="${CSOM_DIR:-/tmp/csom-ref}"
SOMPP_DIR="${SOMPP_DIR:-/tmp/sompp-ref}"
PYSOM_DIR="${PYSOM_DIR:-/tmp/pysom-ref}"
TRUFFLESOM_DIR="${TRUFFLESOM_DIR:-/tmp/trufflesom}"
MX_DIR="${MX_DIR:-/tmp/mx}"

bench=(Sieve Permute Towers Queens List Storage Bounce BubbleSort QuickSort TreeSort Fannkuch Mandelbrot)

declare -A INNER=( [Mandelbrot]=200 [Fannkuch]=7 )
declare -A OUTER_OVERRIDE=( [Mandelbrot]=1 [Fannkuch]=5 )

# Engine-reported runtime (seconds) from one process run.
parse_inner() {
    awk '
        /iterations=[0-9]+ runtime: [0-9]+us/  { for (i=1;i<=NF;i++) if ($i ~ /us$/) { sub("us","",$i); printf "%.3f\n", $i/1e6; exit } }
        /average: [0-9]+us total: [0-9]+us/    { for (i=1;i<=NF;i++) if ($i == "total:") { gsub("us","",$(i+1)); printf "%.3f\n", $(i+1)/1e6; exit } }
    '
}

best_of_3() {
    local cmd="$1"
    local out best
    best=""
    for i in 1 2 3; do
        if [ "$MODE" = wall ]; then
            out=$(/usr/bin/time -f '%e' bash -c "$cmd >/dev/null" 2>&1 1>/dev/null | tail -1 || true)
        else
            out=$(bash -c "$cmd" 2>/dev/null | parse_inner || true)
        fi
        [ -z "$out" ] && continue
        if [ -z "$best" ] || awk -v a="$out" -v b="$best" 'BEGIN { exit !(a<b) }'; then
            best="$out"
        fi
    done
    echo "$best"
}

# Header
echo "# Mode: $MODE  (inner = system ticks, wall = process wall-clock)"
hf="%-12s | %8s | %8s | %8s | %8s | %10s | %10s | %10s | %8s\n"
printf "$hf" "benchmark" "interp" "aot" "pg" "SOM++" "Truffle" "PySOM-AST" "PySOM-BC" "CSOM"
printf -- "-------------+----------+----------+----------+----------+------------+------------+------------+---------\n"

for b in "${bench[@]}"; do
    inner=${INNER[$b]:-1}
    iters_b=${OUTER_OVERRIDE[$b]:-$ITERS}
    cs="$ASOM_DIR/code_store"

    # asom interp
    asom_int=$(best_of_3 "cd '$ASOM_DIR' && ./asom --plain -cp 'SOM/Smalltalk:SOM/Examples/Benchmarks:test:.' Bench '$b' '$iters_b' '$inner'")

    # asom AOT (bake then time cached)
    rm -rf "$cs" 2>/dev/null
    CCACHE_DISABLE=1 "$ASOM_DIR/asom" -c --preload="$b" \
        -cp "$ASOM_DIR/SOM/Smalltalk:$ASOM_DIR/SOM/Examples/Benchmarks:$ASOM_DIR/test:$ASOM_DIR" \
        Bench "$b" 1 "$inner" >/dev/null 2>&1 || true
    asom_aot=$(best_of_3 "cd '$ASOM_DIR' && ./asom -cp 'SOM/Smalltalk:SOM/Examples/Benchmarks:test:.' Bench '$b' '$iters_b' '$inner'")

    # asom PG (post-run bake then time)
    rm -rf "$cs" 2>/dev/null
    CCACHE_DISABLE=1 "$ASOM_DIR/asom" -p --pg-threshold=1 --preload="$b" \
        -cp "$ASOM_DIR/SOM/Smalltalk:$ASOM_DIR/SOM/Examples/Benchmarks:$ASOM_DIR/test:$ASOM_DIR" \
        Bench "$b" 5 "$inner" >/dev/null 2>&1 || true
    asom_pg=$(best_of_3 "cd '$ASOM_DIR' && ./asom -cp 'SOM/Smalltalk:SOM/Examples/Benchmarks:test:.' Bench '$b' '$iters_b' '$inner'")
    rm -rf "$cs" 2>/dev/null

    sompp_t=$(best_of_3 "cd '$SOMPP_DIR' && ./SOM++ -cp 'Smalltalk:Examples/Benchmarks' Examples/Benchmarks/BenchmarkHarness.som '$b' '$iters_b' '$inner'")
    truf_t=$(best_of_3 "cd '$TRUFFLESOM_DIR' && JVMCI_VERSION_CHECK=ignore PATH='$MX_DIR':\$PATH ./som -cp 'Smalltalk:Examples/Benchmarks' Examples/Benchmarks/BenchmarkHarness.som '$b' '$iters_b' 0 '$inner'")
    pysomA_t=$(best_of_3 "cd '$PYSOM_DIR' && SOM_INTERP=AST PYTHONPATH=src python3 src/main.py -cp 'core-lib/Smalltalk:core-lib/Examples/Benchmarks' core-lib/Examples/Benchmarks/BenchmarkHarness.som '$b' '$iters_b' '$inner'")
    pysomB_t=$(best_of_3 "cd '$PYSOM_DIR' && SOM_INTERP=BC  PYTHONPATH=src python3 src/main.py -cp 'core-lib/Smalltalk:core-lib/Examples/Benchmarks' core-lib/Examples/Benchmarks/BenchmarkHarness.som '$b' '$iters_b' '$inner'")
    csom_t=$(best_of_3 "cd '$CSOM_DIR' && ./CSOM -cp 'Smalltalk:Examples/Benchmarks' Examples/Benchmarks/BenchmarkHarness.som '$b' '$iters_b' 0 '$inner'")

    printf "$hf" "$b" \
        "${asom_int:-?}" "${asom_aot:-?}" "${asom_pg:-?}" \
        "${sompp_t:-?}" "${truf_t:-?}" \
        "${pysomA_t:-?}" "${pysomB_t:-?}" "${csom_t:-?}"
done
