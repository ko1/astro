#!/usr/bin/env bash
# Triage: for every bench, report alloc_bytes / gc_count / elapsed under the
# CURRENT koruby_precise binary (--plain, KORUBY_GC_STATS=1) and validate output
# == CRuby.  Deterministic alloc/gc_count identifies which benches stress GC.
# Usage: tools/gc_triage.sh [bench_dir]
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE" || exit 1
BD=${1:-../rubyharness/bench}
BIN="$HERE/koruby_precise"
printf '%-16s %10s %7s %7s %7s %9s  %s\n' bench allocMB gc minor major elapsed status
for f in "$BD"/*.rb; do
  b=$(basename "$f" .rb)
  out=$(KORUBY_GC_STATS=1 timeout 120 "$BIN" --plain "$f" 2>/tmp/gcstats.txt)
  rc=$?
  ref=$(timeout 120 ruby "$f" 2>/dev/null)
  st="ok"; [ "$out" != "$ref" ] && st="DIFF"; [ $rc -ne 0 ] && st="rc=$rc"
  line=$(grep "__KORUBY_GC__" /tmp/gcstats.txt)
  ab=$(echo "$line" | sed -n 's/.*alloc_bytes=\([0-9]*\).*/\1/p')
  gc=$(echo "$line" | sed -n 's/.*gc_count=\([0-9]*\).*/\1/p')
  mi=$(echo "$line" | sed -n 's/.*minor=\([0-9]*\).*/\1/p')
  ma=$(echo "$line" | sed -n 's/.*major=\([0-9]*\).*/\1/p')
  el=$(echo "$line" | sed -n 's/.*elapsed=\([0-9.]*\).*/\1/p')
  ambytes=${ab:-0}
  printf '%-16s %10.1f %7s %7s %7s %9s  %s\n' "$b" "$(awk -v x="$ambytes" 'BEGIN{print x/1048576}')" "${gc:-?}" "${mi:-?}" "${ma:-?}" "${el:-?}" "$st"
done
