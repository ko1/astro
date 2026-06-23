#!/usr/bin/env bash
# Run the corpus under STRESS+PURGE for each GC backend, recording crash/timeout
# counts + the offending files.  Surfaces backend-specific moving-GC bugs.
# Usage: tools/gc_stress_sweep.sh [backend...]
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE" || exit 1
BACKENDS=("$@")
[ ${#BACKENDS[@]} -eq 0 ] && BACKENDS=(copy copy_gen immix immix_gen mark mark_gen mark_compact mark_compact_gen)
OUT="bench-report/$(date +%Y%m%d-%H%M%S)-gc-stress.txt"
echo "# GC STRESS+PURGE corpus sweep" | tee "$OUT"
for gc in "${BACKENDS[@]}"; do
  rm -f koruby_precise
  if ! make GC="$gc" >/tmp/gcs_build_$gc.log 2>&1; then echo "$gc BUILD-FAIL" | tee -a "$OUT"; continue; fi
  log=/tmp/gcs_stress_$gc.txt
  make test STRESS=1 > "$log" 2>&1
  cat=$(grep -E "^TOTAL" "$log" | head -1)
  crashfiles=$(grep -A50 "recovered CRASH\|WHOLE_CRASH\|TIMEOUT" "$log" | grep -oE "method/[a-z]+_[0-9]+\.rb" | sort -u | tr '\n' ' ')
  printf '%-18s %s\n   files: %s\n' "$gc" "$cat" "${crashfiles:-none}" | tee -a "$OUT"
done
echo "saved: $OUT" | tee -a "$OUT"
