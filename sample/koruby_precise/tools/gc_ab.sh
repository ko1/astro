#!/usr/bin/env bash
# Alternating (round-robin) GC backend A/B for optcarrot, robust to fluctuating
# machine load: build+bake every backend ONCE into saved binary+code_store, then
# run them round-robin so each backend samples the same load distribution.
# Per backend report best + median fps.
# Usage: tools/gc_ab.sh [FRAMES] [ROUNDS] [backend...]
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE" || exit 1
FRAMES=${1:-180}
ROUNDS=${2:-7}
shift 2 2>/dev/null || true
BACKENDS=("$@")
[ ${#BACKENDS[@]} -eq 0 ] && BACKENDS=(copy copy_gen immix immix_gen mark_gen)
OPT="$HERE/../abruby/benchmark/optcarrot"
BUNDLE=/tmp/optc_bundle.rb
WORK=/tmp/gc_ab; mkdir -p "$WORK"
OPTC_MODE=build tools/optcarrot.sh "$FRAMES" >/dev/null 2>&1
OUT="bench-report/$(date +%Y%m%d-%H%M%S)-gc-ab-optcarrot.txt"
echo "# optcarrot AOT GC A/B (alternating)  FRAMES=$FRAMES ROUNDS=$ROUNDS" | tee "$OUT"

# --- build phase: one binary + one code_store per backend (REUSE=1 skips if cached) ---
declare -a OK=()
for gc in "${BACKENDS[@]}"; do
  if [ "${REUSE:-0}" = 1 ] && [ -x "$WORK/bin_$gc" ] && [ -d "$WORK/cs_$gc" ]; then OK+=("$gc"); continue; fi
  if ! make GC="$gc" >/tmp/gc_build_$gc.log 2>&1; then echo "$gc BUILD-FAIL" | tee -a "$OUT"; continue; fi
  cp -f koruby_precise "$WORK/bin_$gc" || continue
  ( cd "$OPT" && rm -rf code_store && timeout 240 env CCACHE_DISABLE=1 "$WORK/bin_$gc" --aot-compile "$BUNDLE" ) >/tmp/gc_bake_$gc.log 2>&1
  if [ $? -ne 0 ]; then echo "$gc BAKE-FAIL" | tee -a "$OUT"; continue; fi
  rm -rf "$WORK/cs_$gc"; cp -r "$OPT/code_store" "$WORK/cs_$gc" || continue
  OK+=("$gc")
done
echo "# built: ${OK[*]}" | tee -a "$OUT"

# --- measure phase: round-robin ---
declare -A SAMPLES=()
for r in $(seq 1 "$ROUNDS"); do
  for gc in "${OK[@]}"; do
    rm -rf "$OPT/code_store"; cp -r "$WORK/cs_$gc" "$OPT/code_store"
    line=$( cd "$OPT" && timeout 200 "$WORK/bin_$gc" --compiled-only "$BUNDLE" 2>/dev/null )
    f=$(echo "$line" | sed -n 's/^fps: //p')
    c=$(echo "$line" | sed -n 's/^checksum: //p')
    [ "$c" != "59662" ] && f=""   # invalidate runs with wrong/missing checksum
    [ -n "$f" ] && SAMPLES[$gc]="${SAMPLES[$gc]:-} $f"
  done
  echo "# round $r done (load: $(cut -d' ' -f1 /proc/loadavg))" | tee -a "$OUT"
done

# --- report best + median ---
printf '%-20s %9s %9s %4s\n' "backend" "best_fps" "median" "n" | tee -a "$OUT"
for gc in "${OK[@]}"; do
  echo "${SAMPLES[$gc]:-}" | tr ' ' '\n' | grep -E '[0-9]' | sort -n > "$WORK/s_$gc"
  n=$(wc -l < "$WORK/s_$gc")
  best=$(tail -1 "$WORK/s_$gc"); [ -z "$best" ] && best=0
  med=$(awk 'NR==FNR{a[NR]=$1;c=NR;next}END{print (c? a[int((c+1)/2)] : 0)}' "$WORK/s_$gc" "$WORK/s_$gc")
  printf '%-20s %9.2f %9.2f %4s\n' "$gc" "$best" "$med" "$n" | tee -a "$OUT"
done
echo "saved: $OUT"
