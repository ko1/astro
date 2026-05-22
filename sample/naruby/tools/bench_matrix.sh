#!/bin/bash
# naruby bench matrix with the new attribute/action flags
# (--plain / --aot-compile --run / --pg-compile / -b skip-bake).
# Outputs TSV: bench, plain, aot-1st, aot-c, pg-1st, pg-c, lto-1st, lto-c.
#
#   plain   = ./naruby --plain $B                       (interp, no code store)
#   aot-1st = ./naruby --ccs --aot-compile --run $B     (cold AOT bake + run)
#   aot-c   = ./naruby -b $B                            (cached SDs, skip bake)
#   pg-1st  = ./naruby --ccs --pg-compile $B            (interp + PGSD bake)
#   pg-c    = ./naruby --pg-compile -b $B               (cached PGSDs, skip bake)
#   lto-*   = same as pg-* but ASTRO_EXTRA_*=-flto
#
# Note: pg-1st intentionally has NO --aot-compile.  Mixing AOT with PG
# would put extra AOT SDs (HORG-keyed) into all.so alongside the PGSDs
# (HOPT-keyed) we actually want.  PG-only (= run interp + bake PGSDs)
# yields an all.so with only profile-baked entries.

set -eu

cd /home/ko1/ruby/astro/sample/naruby

BENCHES=(fib ackermann tak gcd loop call chain20 chain40 chain_add compose branch_dom deep_const collatz early_return prime_count)

median() {
    printf "%s\n" "$@" | sort -n | awk -v n=$# 'NR==int((n+1)/2){print}'
}

# Cold run: clear store, time the full invocation.  3 runs, median.
# $1 = lto:0|1, $2 = naruby flags (incl --ccs), $3 = bench name
run_cold() {
    local lto=$1; shift
    local flags="$1"; shift
    local b="$1"; shift
    local times=()
    for i in 1 2 3; do
        rm -rf code_store
        if [ "$lto" = "1" ]; then
            t=$(CCACHE_DISABLE=1 ASTRO_EXTRA_CFLAGS="-flto" \
                ASTRO_EXTRA_LDFLAGS="-Wl,-Bsymbolic -flto" \
                /usr/bin/time -f "%e" ./naruby --ccs $flags --quiet bench/$b.na.rb 2>&1 >/dev/null | tail -1)
        else
            t=$(CCACHE_DISABLE=1 \
                /usr/bin/time -f "%e" ./naruby --ccs $flags --quiet bench/$b.na.rb 2>&1 >/dev/null | tail -1)
        fi
        times+=("$t")
    done
    median "${times[@]}"
}

# Cached run: don't clear store, skip bake.  5 runs, median.
# $1 = naruby flags ("-b" or "--pg-compile -b"), $2 = bench name
run_cached() {
    local flags="$1"; shift
    local b="$1"; shift
    local times=()
    for i in 1 2 3 4 5; do
        t=$(/usr/bin/time -f "%e" ./naruby $flags --quiet bench/$b.na.rb 2>&1 >/dev/null | tail -1)
        times+=("$t")
    done
    median "${times[@]}"
}

printf "bench\tplain\taot-1st\taot-c\tpg-1st\tpg-c\tlto-1st\tlto-c\n"
for b in "${BENCHES[@]}"; do
    plain=$(run_cached "--plain"            "$b")

    aot1=$(run_cold   0 "--aot-compile --run" "$b")
    aotc=$(run_cached "-b"                  "$b")

    pg1=$(run_cold    0 "--pg-compile"      "$b")
    pgc=$(run_cached  "--pg-compile -b"     "$b")

    lto1=$(run_cold   1 "--pg-compile"      "$b")
    ltoc=$(run_cached "--pg-compile -b"     "$b")

    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
        "$b" "$plain" "$aot1" "$aotc" "$pg1" "$pgc" "$lto1" "$ltoc"
done
