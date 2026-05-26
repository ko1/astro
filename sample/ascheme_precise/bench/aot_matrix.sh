#!/bin/bash
# bench v7: plain + aot-cached × 17 precise backend (+ libgc plain baseline)
# 注: libgc ascheme は --aot-compile 未対応 (plain のみ)。 ascheme_precise の
# 全 17 backend を build-switch して 9 workload × {plain, aot-cached} を計測。
# Usage: ./aot_matrix.sh [> result.txt]  (= 100 分前後で完走)
set -uo pipefail
# Run from sample/ regardless of how it was invoked.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/../.."

declare -A EXPECTED
EXPECTED[fib35]="9227465"
EXPECTED[sumloop]="4999999950000000"
EXPECTED[nbody]="-0.169078070659343"
EXPECTED[sieve_big]="664579"
EXPECTED[deriv]="3"
EXPECTED[nqueens]="14200"
EXPECTED[fannkuch]="30"
EXPECTED[cps_loop]="499999500000"
EXPECTED[matmul]="1087492"

declare -A CATEGORY
INTEGER_BENCHES=(fib35 sumloop nbody)
GC_BENCHES=(sieve_big deriv)
MIXED_BENCHES=(nqueens fannkuch cps_loop matmul)
for b in "${INTEGER_BENCHES[@]}"; do CATEGORY[$b]=INT; done
for b in "${GC_BENCHES[@]}";      do CATEGORY[$b]=GC;  done
for b in "${MIXED_BENCHES[@]}";   do CATEGORY[$b]=MIX; done
BENCHES=("${INTEGER_BENCHES[@]}" "${GC_BENCHES[@]}" "${MIXED_BENCHES[@]}")

BACKENDS=(none bump mark mark_gen mark_gen_inc mark_freelist mark_bitmap_gen mark_card_gen copy copy_gen mark_compact mark_compact_gen mark_bump_gen immix immix_gen)
BENCH_PATH="ascheme/bench/big"

run_validated() {
    local bin="$1"; local script="$2"; local expected="$3"; shift 3
    local extra=("$@")
    local best=""
    for i in 1 2 3; do
        local outfile=$(mktemp); local timefile=$(mktemp)
        /usr/bin/time -f "%e" -o "$timefile" timeout 180 "$bin" -q "${extra[@]}" "$script" > "$outfile" 2>&1
        local rc=$?
        local first_line=$(head -1 "$outfile" 2>/dev/null)
        local t=$(cat "$timefile" 2>/dev/null)
        rm -f "$outfile" "$timefile"
        if [ "$rc" -ne 0 ] || [ "$first_line" != "$expected" ]; then
            echo "FAIL"; return
        fi
        if [ -z "$best" ]; then best="$t"; fi
        if awk "BEGIN { exit (($t < $best) == 0) }" 2>/dev/null; then best="$t"; fi
    done
    echo "$best"
}

# libgc baseline (plain only — ascheme does not have --aot-compile)
(cd ascheme && make >/dev/null 2>&1)
declare -A LIBGC_PLAIN
for b in "${BENCHES[@]}"; do
    LIBGC_PLAIN[$b]=$(run_validated ascheme/ascheme "$BENCH_PATH/$b.scm" "${EXPECTED[$b]}")
done

declare -A PRECISE_PLAIN PRECISE_AOT
for gc in "${BACKENDS[@]}"; do
    if ! (cd ascheme_precise && make GC="$gc" >/dev/null 2>&1); then
        for b in "${BENCHES[@]}"; do
            PRECISE_PLAIN[$gc:$b]=BLD; PRECISE_AOT[$gc:$b]=BLD
        done
        continue
    fi
    for b in "${BENCHES[@]}"; do
        expected="${EXPECTED[$b]}"
        PRECISE_PLAIN[$gc:$b]=$(run_validated ascheme_precise/ascheme_precise "$BENCH_PATH/$b.scm" "$expected")
        # Build AOT (= clear-cs then aot-compile). astro_cs_init uses CWD
        # for the code_store dir, so we run from ascheme_precise/ to keep
        # the cache out of sample/. Some backend × bench combos hang in
        # GC sweep (e.g. mark_freelist + fib35) — 60s timeout limits damage.
        (cd ascheme_precise && rm -rf code_store && CCACHE_DISABLE=1 timeout 180 ./ascheme_precise -q --clear-cs --aot-compile "bench/big/$b.scm" >/dev/null 2>&1)
        if [ -f ascheme_precise/code_store/all.so ]; then
            # Run with --aot-compile (= load cached SDs + run). We want the
            # cache from the cd'd build above, so the run must also start
            # from ascheme_precise/ for cwd-relative code_store/ to resolve.
            best=""
            for i in 1 2 3; do
                outfile=$(mktemp); timefile=$(mktemp)
                (cd ascheme_precise && /usr/bin/time -f "%e" -o "$timefile" timeout 180 ./ascheme_precise -q --aot-compile "bench/big/$b.scm") > "$outfile" 2>&1
                rc=$?
                first_line=$(head -1 "$outfile" 2>/dev/null)
                t=$(cat "$timefile" 2>/dev/null)
                rm -f "$outfile" "$timefile"
                if [ "$rc" -ne 0 ] || [ "$first_line" != "$expected" ]; then
                    best=FAIL; break
                fi
                if [ -z "$best" ]; then best="$t"; fi
                if awk "BEGIN { exit (($t < $best) == 0) }" 2>/dev/null; then best="$t"; fi
            done
            PRECISE_AOT[$gc:$b]="$best"
        else
            PRECISE_AOT[$gc:$b]=NOAOT
        fi
    done
done

# Output table
printf "# bench v7 — plain + aot-cached × 17 precise backend (libgc plain only)\n"
printf "%-12s %-4s %-8s" "bench" "cat" "libgc"
for gc in "${BACKENDS[@]}"; do
    short="${gc//mark_/m_}"; short="${short//compact/c}"
    short="${short//gen/G}"; short="${short//immix/I}"; short="${short//bump/Bu}"
    short="${short//scramble/scr}"; short="${short//bitmap/bmp}"; short="${short//freelist/free}"
    short="${short//card/crd}"
    printf "%-8s%-8s" "${short}_p" "${short}_a"
done
echo
for b in "${BENCHES[@]}"; do
    printf "%-12s %-4s %-8s" "$b" "${CATEGORY[$b]}" "${LIBGC_PLAIN[$b]}"
    for gc in "${BACKENDS[@]}"; do
        printf "%-8s%-8s" "${PRECISE_PLAIN[$gc:$b]}" "${PRECISE_AOT[$gc:$b]}"
    done
    echo
done
