#!/bin/bash
# Deterministic STRESS+PURGE sweep over the fixed sp_spec_list.txt.
# Outputs PASS / FAIL / ERR (summed across files that ran) and CRASH
# (files that SEGV'd / timed out before printing a trailer).
# Usage: tools/sp_stress_sweep.sh [--crashes]   (--crashes lists crashing files)
set +H
cd "$(dirname "$0")/.."
SPEC="${SPEC_BASE:-$HOME/ruby/src/master/spec/ruby/core}"
RUNNER=test/cruby_runner/run_rubyspec.rb
TIMEOUT="${TIMEOUT:-25}"
LIST_CRASHES=0
[ "$1" = "--crashes" ] && LIST_CRASHES=1

PASS=0; FAIL=0; ERR=0; CRASH=0; PROC=0
while IFS= read -r rel; do
  [ -z "$rel" ] && continue
  f="$SPEC/$rel"; [ -f "$f" ] || continue
  PROC=$((PROC + 1))
  out=$(ASTRO_GC_STRESS=1 ASTRO_GC_PURGE=1 timeout "$TIMEOUT" ./koruby_precise "$RUNNER" "$f" 2>&1)
  line=$(echo "$out" | grep -E "_spec.rb: pass=" | tail -1)
  if [ -z "$line" ]; then
    CRASH=$((CRASH + 1))
    [ "$LIST_CRASHES" = 1 ] && echo "CRASH: $rel"
    continue
  fi
  p=$(echo "$line" | sed -E 's/.*pass=([0-9]+).*/\1/')
  fa=$(echo "$line" | sed -E 's/.*fail=([0-9]+).*/\1/')
  er=$(echo "$line" | sed -E 's/.*err=([0-9]+).*/\1/')
  PASS=$((PASS + p)); FAIL=$((FAIL + fa)); ERR=$((ERR + er))
done < tools/sp_spec_list.txt
echo "PASS=$PASS FAIL=$FAIL ERR=$ERR CRASH=$CRASH PROCESSED=$PROC"
