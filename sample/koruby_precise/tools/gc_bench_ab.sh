#!/usr/bin/env bash
# GC backend A/B across a set of benches, robust to fluctuating machine load.
# Builds each backend ONCE (saved binary), then runs every (bench,backend) pair
# round-robin for ROUNDS rounds so each backend samples the same load.  Per
# (bench,backend) records: min elapsed, gc_count, min gc_seconds, alloc, checksum
# validity.  Output: a TSV at $OUT plus readable tables.  --plain mode (GC
# behaviour = alloc/count is identical to AOT; AOT only changes the mutator/GC
# time split).  REUSE=1 skips builds if /tmp/gc_ab/bin_<gc> already exists.
# Usage: tools/gc_bench_ab.sh "<backends>" "<benches>" [ROUNDS]
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE" || exit 1
BACKENDS=(${1:?backends}); BENCHES=(${2:?benches}); ROUNDS=${3:-3}
BD="$HERE/../rubyharness/bench"
WORK=/tmp/gc_ab; mkdir -p "$WORK"
OUT="bench-report/$(date +%Y%m%d-%H%M%S)-gc-bench.tsv"
: > "$OUT"

# build phase
declare -a OK=()
for gc in "${BACKENDS[@]}"; do
  if [ "${REUSE:-0}" = 1 ] && [ -x "$WORK/bin_$gc" ]; then OK+=("$gc"); continue; fi
  if make GC="$gc" >/tmp/gc_build_$gc.log 2>&1; then cp -f koruby_precise "$WORK/bin_$gc"; OK+=("$gc");
  else echo "# $gc BUILD-FAIL"; fi
done
echo "# backends: ${OK[*]}"
echo "# benches: ${BENCHES[*]}"

# reference outputs (CRuby) for checksum validation
declare -A REF
for b in "${BENCHES[@]}"; do REF[$b]=$(timeout 120 ruby "$BD/$b.rb" 2>/dev/null); done

# measure phase: round-robin, keep min elapsed + min gc_seconds, gc_count is deterministic
declare -A E G S A C   # min elapsed, gc_count, min gc_seconds, allocMB, checksum-ok
for r in $(seq 1 "$ROUNDS"); do
  for b in "${BENCHES[@]}"; do
    for gc in "${OK[@]}"; do
      out=$(KORUBY_GC_STATS=1 timeout 150 "$WORK/bin_$gc" --plain "$BD/$b.rb" 2>/tmp/gs.txt)
      ln=$(grep "__KORUBY_GC__" /tmp/gs.txt)
      [ "$out" = "${REF[$b]}" ] && ok=1 || ok=0
      el=$(echo "$ln" | sed -n 's/.*elapsed=\([0-9.]*\).*/\1/p')
      gc_c=$(echo "$ln" | sed -n 's/.*gc_count=\([0-9]*\).*/\1/p')
      gs=$(echo "$ln" | sed -n 's/.*gc_seconds=\([0-9.]*\).*/\1/p')
      ab=$(echo "$ln" | sed -n 's/.*alloc_bytes=\([0-9]*\).*/\1/p')
      key="$b/$gc"
      C[$key]=$ok; G[$key]=${gc_c:-?}
      A[$key]=$(awk -v x="${ab:-0}" 'BEGIN{printf "%.1f", x/1048576}')
      if [ -n "$el" ]; then
        cur=${E[$key]:-}; [ -z "$cur" ] && E[$key]=$el || E[$key]=$(awk -v a="$el" -v b="$cur" 'BEGIN{print (a<b)?a:b}')
      fi
      if [ -n "$gs" ]; then
        cur=${S[$key]:-}; [ -z "$cur" ] && S[$key]=$gs || S[$key]=$(awk -v a="$gs" -v b="$cur" 'BEGIN{print (a<b)?a:b}')
      fi
    done
  done
  echo "# round $r done (load $(cut -d' ' -f1 /proc/loadavg))"
done

# emit TSV
echo -e "bench\tbackend\tmin_elapsed\tgc_count\tmin_gc_seconds\tallocMB\tchecksum_ok" >> "$OUT"
for b in "${BENCHES[@]}"; do for gc in "${OK[@]}"; do k="$b/$gc";
  echo -e "$b\t$gc\t${E[$k]:-NA}\t${G[$k]:-NA}\t${S[$k]:-NA}\t${A[$k]:-NA}\t${C[$k]:-0}" >> "$OUT"
done; done
echo "saved: $OUT"
