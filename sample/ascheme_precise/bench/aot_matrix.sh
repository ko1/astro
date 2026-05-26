#!/bin/bash
# bench v8: plain + aot-cached × 15 precise backend (+ libgc plain baseline)
# 注:
#   - libgc ascheme は --aot-compile 未対応 (plain のみ)
#   - copy_gen_inc は backend として削除 (commit e60fa150)、 列から除外
#   - copy_scramble は audit 専用 backend、 matrix からは除外
#   - /usr/bin/time -f "%e %M" で wallclock 秒 + peak RSS (KB) を同時取得
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
# 15 backend (= 11 practical + bump/none/mark_bump_gen/mark_freelist の特殊用途 4)。
# copy_gen_inc / copy_scramble は除外。
# Override via env: BENCH_BACKENDS="copy mark" bash aot_matrix.sh — useful for chunked runs.
if [ -n "${BENCH_BACKENDS:-}" ]; then
    read -ra BACKENDS <<< "$BENCH_BACKENDS"
fi
# Override libgc baseline: set BENCH_SKIP_LIBGC=1 to skip (= already measured).
SKIP_LIBGC="${BENCH_SKIP_LIBGC:-0}"
BENCH_PATH="ascheme/bench/big"

# Runs the binary 3× with /usr/bin/time -f "%e %M".  Outputs two whitespace-
# separated tokens: "<median_elapsed_sec> <max_rss_kb>" on success, or "FAIL FAIL".
# Median is taken from the 3 elapsed samples (sorted, middle).  RSS is the max of
# the 3 samples.
run_validated() {
    local bin="$1"; local script="$2"; local expected="$3"; shift 3
    local extra=("$@")
    local elapsed=() rss=()
    for i in 1 2 3; do
        local outfile timefile
        outfile=$(mktemp); timefile=$(mktemp)
        /usr/bin/time -f "%e %M" -o "$timefile" timeout 180 "$bin" -q "${extra[@]}" "$script" > "$outfile" 2>&1
        local rc=$?
        local first_line; first_line=$(head -1 "$outfile" 2>/dev/null)
        # Take the LAST line of the time file (some failures prepend "Command exited..." text).
        local t_line; t_line=$(grep -E '^[0-9]+(\.[0-9]+)?[[:space:]]+[0-9]+$' "$timefile" 2>/dev/null | tail -1)
        local t; t=${t_line%% *}
        local m; m=${t_line##* }
        rm -f "$outfile" "$timefile"
        if [ "$rc" -ne 0 ] || [ "$first_line" != "$expected" ]; then
            echo "FAIL FAIL"; return
        fi
        elapsed+=("$t"); rss+=("$m")
    done
    # median elapsed (3 samples → middle after sort)
    local med
    med=$(printf "%s\n" "${elapsed[@]}" | sort -g | sed -n '2p')
    # max RSS
    local maxr
    maxr=$(printf "%s\n" "${rss[@]}" | sort -n | tail -1)
    echo "$med $maxr"
}

# Same as run_validated but for AOT-cached runs: the binary must be invoked from
# ascheme_precise/ (cwd-relative code_store/).  Validates first-line output.
run_validated_aot() {
    local script_rel="$1"; local expected="$2"
    local elapsed=() rss=()
    for i in 1 2 3; do
        local outfile timefile
        outfile=$(mktemp); timefile=$(mktemp)
        (cd ascheme_precise && /usr/bin/time -f "%e %M" -o "$timefile" timeout 180 ./ascheme_precise -q --aot-compile "$script_rel") > "$outfile" 2>&1
        local rc=$?
        local first_line; first_line=$(head -1 "$outfile" 2>/dev/null)
        local t_line; t_line=$(grep -E '^[0-9]+(\.[0-9]+)?[[:space:]]+[0-9]+$' "$timefile" 2>/dev/null | tail -1)
        local t; t=${t_line%% *}
        local m; m=${t_line##* }
        rm -f "$outfile" "$timefile"
        if [ "$rc" -ne 0 ] || [ "$first_line" != "$expected" ]; then
            echo "FAIL FAIL"; return
        fi
        elapsed+=("$t"); rss+=("$m")
    done
    local med; med=$(printf "%s\n" "${elapsed[@]}" | sort -g | sed -n '2p')
    local maxr; maxr=$(printf "%s\n" "${rss[@]}" | sort -n | tail -1)
    echo "$med $maxr"
}

# libgc baseline (plain only — ascheme does not have --aot-compile)
declare -A LIBGC_PLAIN_T LIBGC_PLAIN_R
if [ "$SKIP_LIBGC" = "0" ]; then
    (cd ascheme && make >/dev/null 2>&1)
    for b in "${BENCHES[@]}"; do
        read -r t r < <(run_validated ascheme/ascheme "$BENCH_PATH/$b.scm" "${EXPECTED[$b]}")
        LIBGC_PLAIN_T[$b]=$t
        LIBGC_PLAIN_R[$b]=$r
    done
fi

declare -A PRECISE_PLAIN_T PRECISE_PLAIN_R PRECISE_AOT_T PRECISE_AOT_R
for gc in "${BACKENDS[@]}"; do
    # Force rebuild: the marker-file mechanism can leave a stale binary if the
    # binary's mtime is newer than .built_gc's update.  rm-ing makes rebuild
    # unconditional.
    rm -f ascheme_precise/ascheme_precise
    if ! (cd ascheme_precise && make GC="$gc" >/dev/null 2>&1); then
        for b in "${BENCHES[@]}"; do
            PRECISE_PLAIN_T[$gc:$b]=BLD; PRECISE_PLAIN_R[$gc:$b]=BLD
            PRECISE_AOT_T[$gc:$b]=BLD;   PRECISE_AOT_R[$gc:$b]=BLD
        done
        continue
    fi
    for b in "${BENCHES[@]}"; do
        expected="${EXPECTED[$b]}"
        read -r t r < <(run_validated ascheme_precise/ascheme_precise "$BENCH_PATH/$b.scm" "$expected")
        PRECISE_PLAIN_T[$gc:$b]=$t; PRECISE_PLAIN_R[$gc:$b]=$r
        # Build AOT (= clear-cs then aot-compile).  astro_cs_init uses CWD
        # for the code_store dir, so we run from ascheme_precise/ to keep
        # the cache out of sample/.  Some backend × bench combos hang in
        # GC sweep (e.g. mark_freelist + fib35) — 180s timeout limits damage.
        (cd ascheme_precise && rm -rf code_store && CCACHE_DISABLE=1 timeout 180 ./ascheme_precise -q --clear-cs --aot-compile "bench/big/$b.scm" >/dev/null 2>&1)
        if [ -f ascheme_precise/code_store/all.so ]; then
            read -r t r < <(run_validated_aot "bench/big/$b.scm" "$expected")
            PRECISE_AOT_T[$gc:$b]=$t; PRECISE_AOT_R[$gc:$b]=$r
        else
            PRECISE_AOT_T[$gc:$b]=NOAOT; PRECISE_AOT_R[$gc:$b]=NOAOT
        fi
    done
done

fmt_rss() {
    # KB → MiB (1 decimal place) if numeric, else passthrough.
    local v="$1"
    if [[ "$v" =~ ^[0-9]+$ ]]; then
        awk -v k="$v" 'BEGIN{printf "%.1f", k/1024.0}'
    else
        echo "$v"
    fi
}

# Output: 2 tables.  Same row order, same column order, but separate
# elapsed-time / peak-RSS-MiB tables (= easier to scan than interleaved).
printf "# bench v8 — plain + aot-cached × 15 precise backend (libgc plain only)\n"
printf "# columns suffix: _p = plain, _a = aot-cached\n\n"

print_header() {
    printf "%-12s %-4s %-8s" "bench" "cat" "libgc"
    for gc in "${BACKENDS[@]}"; do
        short="${gc//mark_/m_}"; short="${short//compact/c}"
        short="${short//gen/G}"; short="${short//immix/I}"; short="${short//bump/Bu}"
        short="${short//scramble/scr}"; short="${short//bitmap/bmp}"; short="${short//freelist/free}"
        short="${short//card/crd}"
        printf "%-8s%-8s" "${short}_p" "${short}_a"
    done
    echo
}

printf "## elapsed (seconds, median of 3)\n"
print_header
for b in "${BENCHES[@]}"; do
    printf "%-12s %-4s %-8s" "$b" "${CATEGORY[$b]}" "${LIBGC_PLAIN_T[$b]}"
    for gc in "${BACKENDS[@]}"; do
        printf "%-8s%-8s" "${PRECISE_PLAIN_T[$gc:$b]}" "${PRECISE_AOT_T[$gc:$b]}"
    done
    echo
done
echo

printf "## peak RSS (MiB, max of 3 /usr/bin/time -M)\n"
print_header
for b in "${BENCHES[@]}"; do
    printf "%-12s %-4s %-8s" "$b" "${CATEGORY[$b]}" "$(fmt_rss "${LIBGC_PLAIN_R[$b]}")"
    for gc in "${BACKENDS[@]}"; do
        printf "%-8s%-8s" "$(fmt_rss "${PRECISE_PLAIN_R[$gc:$b]}")" "$(fmt_rss "${PRECISE_AOT_R[$gc:$b]}")"
    done
    echo
done
