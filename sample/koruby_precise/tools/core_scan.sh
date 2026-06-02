#!/bin/bash
# Scan ALL of rubyspec/core (not just sp_spec_list.txt) and classify each
# *_spec.rb by how it exits.  Goal: drive koruby_precise toward "all of core
# runs without crashing", incrementally.
#
#   SEGV  = rc 132..139 (real crash — the priority)
#   TIMEO = rc 124 (timed out — hang or slowness)
#   NOTR  = exited, no `pass=` trailer (load error / feature gap / clean abort)
#   OK    = printed a trailer (ran to completion; may have FAIL/ERR inside)
#
# Default: NORMAL mode (fast).  Set STRESS=1 to run under ASTRO_GC_STRESS+PURGE.
# Usage: tools/core_scan.sh [--segv|--timeo|--notr]   (filter the printed list)
set +H
cd "$(dirname "$0")/.."
SPEC="${SPEC_BASE:-$HOME/ruby/src/master/spec/ruby/core}"
RUNNER=test/cruby_runner/run_rubyspec.rb
TIMEOUT="${TIMEOUT:-15}"
ENVV=()
[ "${STRESS:-0}" = 1 ] && ENVV=(ASTRO_GC_STRESS=1 ASTRO_GC_PURGE=1)
FILTER="${1:-}"

SEGV=0; TIMEO=0; NOTR=0; OKC=0; PROC=0
OUT="$(pwd)/tools/.gc_run"; mkdir -p "$OUT"
: > "$OUT/core_segv.txt"; : > "$OUT/core_timeo.txt"; : > "$OUT/core_notr.txt"
while IFS= read -r f; do
  rel="${f#$SPEC/}"
  PROC=$((PROC + 1))
  out=$(env "${ENVV[@]}" timeout "$TIMEOUT" ./koruby_precise "$RUNNER" "$f" 2>&1)
  rc=$?
  if echo "$out" | grep -q "_spec.rb: pass="; then
    OKC=$((OKC + 1)); continue
  fi
  if [ "$rc" -ge 132 ] && [ "$rc" -le 139 ]; then
    SEGV=$((SEGV + 1)); echo "$rel" >> "$OUT/core_segv.txt"
    [ "$FILTER" = "--segv" ] && echo "SEGV: $rel (rc=$rc)"
  elif [ "$rc" -eq 124 ]; then
    TIMEO=$((TIMEO + 1)); echo "$rel" >> "$OUT/core_timeo.txt"
    [ "$FILTER" = "--timeo" ] && echo "TIMEO: $rel"
  else
    NOTR=$((NOTR + 1)); echo "$rel" >> "$OUT/core_notr.txt"
    [ "$FILTER" = "--notr" ] && echo "NOTR: $rel (rc=$rc)"
  fi
done < <(find "$SPEC" -name '*_spec.rb' | sort)
echo "SEGV=$SEGV TIMEO=$TIMEO NOTR=$NOTR OK=$OKC PROCESSED=$PROC"
echo "(lists in tools/.gc_run/core_{segv,timeo,notr}.txt)"
