#!/bin/bash
# Scan sp_spec_list.txt under STRESS+PURGE, classify each file by exit:
#   SEGV  = rc 139/134/136 (real GC crash — the priority)
#   NOTR  = exited but printed no pass= trailer (feature gap / clean abort)
#   OK    = printed a trailer (ran to completion, may have FAIL/ERR inside)
# Prints SEGV/NOTR file lists (prefixed) then a summary line.
set +H
cd "$(dirname "$0")/.."
SPEC="${SPEC_BASE:-$HOME/ruby/src/master/spec/ruby/core}"
RUNNER=test/cruby_runner/run_rubyspec.rb
TIMEOUT="${TIMEOUT:-25}"
SEGV=0; NOTR=0; OKC=0; PROC=0
while IFS= read -r rel; do
  [ -z "$rel" ] && continue
  f="$SPEC/$rel"; [ -f "$f" ] || continue
  PROC=$((PROC + 1))
  out=$(ASTRO_GC_STRESS=1 ASTRO_GC_PURGE=1 timeout "$TIMEOUT" ./koruby_precise "$RUNNER" "$f" 2>&1)
  rc=$?
  if echo "$out" | grep -q "_spec.rb: pass="; then
    OKC=$((OKC + 1)); continue
  fi
  if [ "$rc" -ge 132 ] && [ "$rc" -le 139 ]; then
    SEGV=$((SEGV + 1)); echo "SEGV: $rel (rc=$rc)"
  else
    NOTR=$((NOTR + 1)); echo "NOTR: $rel (rc=$rc)"
  fi
done < tools/sp_spec_list.txt
echo "SEGV=$SEGV NOTR=$NOTR OK=$OKC PROCESSED=$PROC"
