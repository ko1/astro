#!/usr/bin/env bash
# Unified GC-backend comparison: for each (bench,backend) measure BOTH interp
# (--plain) and AOT (--compiled-only) total time AND GC time, via KORUBY_GC_STATS.
# Best (min) of ROUNDS; gc_count is deterministic.  Needs pre-built per-backend
# binaries in /tmp/gc_ab/bin_<gc> (build them with the loop in the session, or
# `for gc in ...; do make GC=$gc; cp koruby_precise /tmp/gc_ab/bin_$gc; done`).
# Bench-selection criterion: only benches with gc_count>=3 under copy (others do
# ~0 GC so all backends are identical).  Output TSV: bench backend interp_s
# interp_gcs aot_s aot_gcs gc_count ok.
# Usage: gc_full_compare.sh "<backends>" "<benches>" [ROUNDS]
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE" || exit 1
BACKENDS=(${1:?}); BENCHES=(${2:?}); ROUNDS=${3:-3}
BD="$HERE/../rubyharness/bench"; WORK=/tmp/gc_fc; mkdir -p "$WORK"
OUT="bench-report/$(date +%Y%m%d-%H%M%S)-gc-full.tsv"
declare -A REF
for b in "${BENCHES[@]}"; do REF[$b]=$(timeout 120 ruby "$BD/$b.rb" 2>/dev/null); done
echo -e "bench\tbackend\tinterp_s\tinterp_gcs\taot_s\taot_gcs\tgc_count\tok" > "$OUT"
# helper: run a command ROUNDS times, echo "min_elapsed min_gcseconds gc_count out"
run_stats() {  # $@ = command...
  local el="" gs="" gc="?" out="" i ln e g c o
  for i in $(seq 1 "$ROUNDS"); do
    o=$(KORUBY_GC_STATS=1 timeout 200 "$@" 2>/tmp/fc.txt); out="$o"
    ln=$(grep "__KORUBY_GC__" /tmp/fc.txt)
    e=$(echo "$ln" | sed -n 's/.*elapsed=\([0-9.]*\).*/\1/p')
    g=$(echo "$ln" | sed -n 's/.*gc_seconds=\([0-9.]*\).*/\1/p')
    c=$(echo "$ln" | sed -n 's/.*gc_count=\([0-9]*\).*/\1/p'); [ -n "$c" ] && gc="$c"
    el="$el $e"; gs="$gs $g"
  done
  local mine=$(echo "$el" | tr ' ' '\n' | grep -E '[0-9]' | sort -n | head -1)
  local mings=$(echo "$gs" | tr ' ' '\n' | grep -E '[0-9]' | sort -n | head -1)
  printf '%s\t%s\t%s\t%s' "${mine:-NA}" "${mings:-NA}" "$gc" "$out"
}
for b in "${BENCHES[@]}"; do
  for gc in "${BACKENDS[@]}"; do
    bin="/tmp/gc_ab/bin_$gc"; [ -x "$bin" ] || { echo -e "$b\t$gc\tNOBIN" >> "$OUT"; continue; }
    # interp
    IFS=$'\t' read -r is igs icnt iout < <(run_stats "$bin" --plain "$BD/$b.rb")
    # AOT: bake into a per-pair work dir, run from there
    st="$WORK/cs_${gc}_${b}"; rm -rf "$st"; mkdir -p "$st"
    ( cd "$st" && CCACHE_DISABLE=1 timeout 200 "$bin" --aot-compile "$BD/$b.rb" ) >/dev/null 2>&1
    IFS=$'\t' read -r as ags acnt aout < <(cd "$st" && run_stats "$bin" --compiled-only "$BD/$b.rb")
    rm -rf "$st"
    ok=1; [ "$iout" = "${REF[$b]}" ] || ok=0; [ "$aout" = "${REF[$b]}" ] || ok=0
    echo -e "$b\t$gc\t$is\t$igs\t$as\t$ags\t$icnt\t$ok" >> "$OUT"
  done
  echo "# $b done (load $(cut -d' ' -f1 /proc/loadavg))"
done
echo "saved: $OUT"
