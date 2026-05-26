#!/bin/bash
# Minimal bench wrapper for baruby_precise: rebuild + run + capture time+RSS.
# Avoids matrix.rb's BARUBY_GC_STATS env / oracle check / retry complexity.
set -uo pipefail
SAMPLE=/home/ko1/ruby/astro/sample/baruby_precise
BENCHES=(fib fib_pair fannkuch ackermann binary_trees gc_combined hash_chain list_alloc json_parse chain_add)

BENCH_BACKENDS="${BENCH_BACKENDS:-none mark mark_gen mark_gen_inc copy copy_gen mark_compact mark_compact_gen bump mark_bump_gen immix immix_gen mark_bitmap_gen mark_card_gen mark_freelist}"
read -ra BACKENDS <<< "$BENCH_BACKENDS"

REPEATS=3

# libgc baseline (no rebuild, separate binary)
if [ "${BENCH_SKIP_LIBGC:-0}" = "0" ]; then
    LIBGC_BIN=/home/ko1/ruby/astro/sample/baruby/baruby
    if [ -x "$LIBGC_BIN" ]; then
        echo "## libgc baseline"
        for b in "${BENCHES[@]}"; do
            t_min=999; r_max=0; ok=1
            for i in $(seq 1 $REPEATS); do
                tf=$(mktemp); of=$(mktemp)
                /usr/bin/time -f "%e %M" -o "$tf" "$LIBGC_BIN" --plain "$SAMPLE/bench/$b.ba.rb" > "$of" 2>&1
                rc=$?
                t_line=$(grep -E '^[0-9]+(\.[0-9]+)?[[:space:]]+[0-9]+$' "$tf" | tail -1)
                rm -f "$tf" "$of"
                if [ -z "$t_line" ] || [ "$rc" -ne 0 ]; then ok=0; break; fi
                t=${t_line%% *}; r=${t_line##* }
                # numeric min/max via awk
                t_min=$(awk -v a="$t_min" -v b="$t" 'BEGIN{print (b<a)?b:a}')
                r_max=$(awk -v a="$r_max" -v b="$r" 'BEGIN{print (b>a)?b:a}')
            done
            if [ "$ok" = "1" ]; then
                printf "libgc\t%s\t%s\t%s\n" "$b" "$t_min" "$r_max"
            else
                printf "libgc\t%s\tFAIL\tFAIL\n" "$b"
            fi
        done
    fi
fi

# Precise GC backends
for gc in "${BACKENDS[@]}"; do
    # Force rebuild
    rm -f "$SAMPLE/baruby_precise"
    if ! (cd "$SAMPLE" && make -B GC="$gc" ASTRO_DEBUG=0 >/tmp/build_$gc.log 2>&1); then
        echo "## $gc BUILD FAILED" >&2
        for b in "${BENCHES[@]}"; do printf "%s\t%s\tBLD\tBLD\n" "$gc" "$b"; done
        continue
    fi
    if [ ! -x "$SAMPLE/baruby_precise" ]; then
        echo "## $gc binary missing after build" >&2
        for b in "${BENCHES[@]}"; do printf "%s\t%s\tNOBIN\tNOBIN\n" "$gc" "$b"; done
        continue
    fi
    # Verify stamp matches
    stamp=$(strings "$SAMPLE/baruby_precise" 2>/dev/null | grep -oE 'baruby_gc=[a-z_]+' | sort -u | head -1)
    if [ -n "$stamp" ] && [ "$stamp" != "baruby_gc=$gc" ]; then
        echo "## $gc stamp mismatch: $stamp" >&2
    fi
    echo "## $gc"
    for b in "${BENCHES[@]}"; do
        t_min=999; r_max=0; ok=1
        for i in $(seq 1 $REPEATS); do
            tf=$(mktemp); of=$(mktemp)
            /usr/bin/time -f "%e %M" -o "$tf" "$SAMPLE/baruby_precise" --plain "$SAMPLE/bench/$b.ba.rb" > "$of" 2>&1
            rc=$?
            t_line=$(grep -E '^[0-9]+(\.[0-9]+)?[[:space:]]+[0-9]+$' "$tf" | tail -1)
            rm -f "$tf" "$of"
            if [ -z "$t_line" ] || [ "$rc" -ne 0 ]; then ok=0; break; fi
            t=${t_line%% *}; r=${t_line##* }
            t_min=$(awk -v a="$t_min" -v b="$t" 'BEGIN{print (b<a)?b:a}')
            r_max=$(awk -v a="$r_max" -v b="$r" 'BEGIN{print (b>a)?b:a}')
        done
        if [ "$ok" = "1" ]; then
            printf "%s\t%s\t%s\t%s\n" "$gc" "$b" "$t_min" "$r_max"
        else
            printf "%s\t%s\tFAIL\tFAIL\n" "$gc" "$b"
        fi
    done
done
