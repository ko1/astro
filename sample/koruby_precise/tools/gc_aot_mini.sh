#!/usr/bin/env bash
# AOT GC mini-sweep: for a few GC-designed benches, bake + run --compiled-only
# under each backend (cached binaries in /tmp/gc_ab), best-of-3 elapsed + gc
# stats, checksum-validated.  Under AOT the mutator is fast, so GC is a larger
# fraction than --plain — this shows whether a generational backend wins when GC
# actually matters.  Usage: gc_aot_mini.sh "<backends>" "<benches>" [ROUNDS]
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE" || exit 1
BACKENDS=(${1:?}); BENCHES=(${2:?}); ROUNDS=${3:-3}
BD="$HERE/../rubyharness/bench"; WORK=/tmp/gc_ab
declare -A REF; for b in "${BENCHES[@]}"; do REF[$b]=$(ruby "$BD/$b.rb" 2>/dev/null); done
OUT="bench-report/$(date +%Y%m%d-%H%M%S)-gc-aot-mini.tsv"
echo -e "bench\tbackend\tmin_elapsed\tgc_count\tmin_gc_seconds\tchecksum_ok" > "$OUT"
# bake every (bench,backend) once into a per-pair store
for gc in "${BACKENDS[@]}"; do
  [ -x "$WORK/bin_$gc" ] || { echo "# missing bin_$gc"; continue; }
  for b in "${BENCHES[@]}"; do
    st="$WORK/cs_${gc}_${b}"; rm -rf "$st"
    ( cd "$WORK" && rm -rf code_store && CCACHE_DISABLE=1 timeout 180 "$WORK/bin_$gc" --aot-compile "$BD/$b.rb" ) >/dev/null 2>&1
    mv "$WORK/code_store" "$st" 2>/dev/null
  done
done
declare -A E G S C
for r in $(seq 1 "$ROUNDS"); do
  for b in "${BENCHES[@]}"; do for gc in "${BACKENDS[@]}"; do
    st="$WORK/cs_${gc}_${b}"; [ -d "$st" ] || continue
    rm -rf "$WORK/code_store"; cp -r "$st" "$WORK/code_store"
    out=$( cd "$WORK" && KORUBY_GC_STATS=1 timeout 150 "$WORK/bin_$gc" --compiled-only "$BD/$b.rb" 2>/tmp/ga.txt )
    ln=$(grep "__KORUBY_GC__" /tmp/ga.txt)
    [ "$out" = "${REF[$b]}" ] && ok=1 || ok=0
    el=$(echo "$ln" | sed -n 's/.*elapsed=\([0-9.]*\).*/\1/p')
    gc_c=$(echo "$ln" | sed -n 's/.*gc_count=\([0-9]*\).*/\1/p')
    gs=$(echo "$ln" | sed -n 's/.*gc_seconds=\([0-9.]*\).*/\1/p')
    k="$b/$gc"; C[$k]=$ok; G[$k]=${gc_c:-?}
    [ -n "$el" ] && { cur=${E[$k]:-}; [ -z "$cur" ] && E[$k]=$el || E[$k]=$(awk -v a="$el" -v b="$cur" 'BEGIN{print(a<b)?a:b}'); }
    [ -n "$gs" ] && { cur=${S[$k]:-}; [ -z "$cur" ] && S[$k]=$gs || S[$k]=$(awk -v a="$gs" -v b="$cur" 'BEGIN{print(a<b)?a:b}'); }
  done; done
  echo "# aot round $r (load $(cut -d' ' -f1 /proc/loadavg))"
done
for b in "${BENCHES[@]}"; do for gc in "${BACKENDS[@]}"; do k="$b/$gc";
  echo -e "$b\t$gc\t${E[$k]:-NA}\t${G[$k]:-NA}\t${S[$k]:-NA}\t${C[$k]:-0}" >> "$OUT"
done; done
echo "saved: $OUT"
