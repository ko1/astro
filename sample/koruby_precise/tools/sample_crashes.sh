#!/bin/bash
# Sample crash sites of rubyspec files under STRESS+PURGE.
set +H
cd "$(dirname "$0")/.."
SPEC="${SPEC_BASE:-$HOME/ruby/src/master/spec/ruby/core}"
RUNNER=test/cruby_runner/run_rubyspec.rb
LIMIT="${1:-12}"
n=0
while IFS= read -r rel; do
  [ -z "$rel" ] && continue
  f="$SPEC/$rel"
  [ -f "$f" ] || continue
  out=$(ASTRO_GC_STRESS=1 ASTRO_GC_PURGE=1 timeout 25 ./koruby_precise "$RUNNER" "$f" 2>&1)
  if echo "$out" | grep -q "_spec.rb: pass="; then
    continue   # ran fine, no crash
  fi
  site=$(ASTRO_GC_STRESS=1 ASTRO_GC_PURGE=1 timeout 25 gdb -batch -ex run -ex 'bt 2' -ex quit \
           --args ./koruby_precise "$RUNNER" "$f" 2>/dev/null \
           | grep -E '#0 ' | grep -vE 'Download|Thread|libthread' \
           | sed -E 's/\(.*\) at /  @  /' | head -1)
  echo "$(basename "$rel"): $site"
  n=$((n + 1))
  [ "$n" -ge "$LIMIT" ] && break
done < tools/sp_spec_list.txt
