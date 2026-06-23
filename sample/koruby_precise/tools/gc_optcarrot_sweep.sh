#!/usr/bin/env bash
# Sweep optcarrot AOT fps across GC backends.  Rebuilds the binary per backend,
# bakes the code store ONCE, then runs --compiled-only N times, recording best
# fps + checksum.  Usage: tools/gc_optcarrot_sweep.sh [FRAMES] [RUNS] [backend...]
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE" || exit 1
FRAMES=${1:-300}
RUNS=${2:-3}
shift 2 2>/dev/null || true
BACKENDS=("$@")
if [ ${#BACKENDS[@]} -eq 0 ]; then
  BACKENDS=(copy copy_gen immix immix_gen mark_gen mark_compact_gen mark_bitmap_gen mark_card_gen)
fi
OPT="$HERE/../abruby/benchmark/optcarrot"
BIN="$HERE/koruby_precise"
BUNDLE=/tmp/optc_bundle.rb
OPTC_MODE=build tools/optcarrot.sh "$FRAMES" >/dev/null 2>&1   # (re)write bundle
OUT="bench-report/$(date +%Y%m%d-%H%M%S)-gc-optcarrot.txt"
printf '# optcarrot AOT GC sweep  FRAMES=%s RUNS=%s\n' "$FRAMES" "$RUNS" | tee "$OUT"
printf '%-20s %9s  %s\n' "backend" "best_fps" "checksum" | tee -a "$OUT"
for gc in "${BACKENDS[@]}"; do
  if ! make GC="$gc" >/tmp/gc_build_$gc.log 2>&1; then
    printf '%-20s %9s  BUILD-FAIL\n' "$gc" "-" | tee -a "$OUT"; continue
  fi
  ( cd "$OPT" && rm -rf code_store && timeout 180 env CCACHE_DISABLE=1 "$BIN" --aot-compile "$BUNDLE" ) >/tmp/gc_bake_$gc.log 2>&1
  if [ $? -ne 0 ]; then printf '%-20s %9s  BAKE-FAIL\n' "$gc" "-" | tee -a "$OUT"; continue; fi
  best=0; cks="?"
  for r in $(seq 1 "$RUNS"); do
    line=$( cd "$OPT" && timeout 90 "$BIN" --compiled-only "$BUNDLE" 2>/dev/null )
    f=$(echo "$line" | sed -n 's/^fps: //p')
    c=$(echo "$line" | sed -n 's/^checksum: //p')
    [ -n "$c" ] && cks="$c"
    if [ -n "$f" ]; then awk -v a="$f" -v b="$best" 'BEGIN{exit !(a>b)}' && best="$f"; fi
  done
  printf '%-20s %9.2f  %s\n' "$gc" "$best" "$cks" | tee -a "$OUT"
done
echo "saved: $OUT"
