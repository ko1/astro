#!/usr/bin/env bash
# Diff caller(0) output of koruby_precise against CRuby for tools/bt_caller_fixture.rb.
#   tools/bt_caller_cmp.sh [koruby-binary]
# Exit 0 = identical.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
BIN=${1:-$HERE/koruby_precise}
FIX="$HERE/tools/bt_caller_fixture.rb"
OUT=${OUT:-/tmp/claude-1000/-home-ko1-ruby-astro/0a48b788-4b01-46f0-a6f8-490a93bf91eb/scratchpad/bt}
mkdir -p "$OUT"
ruby "$FIX" 2>&1 | sed "s|$HERE/tools/||g" > "$OUT/caller.cruby"
"$BIN" "$FIX" 2>&1 | sed "s|$HERE/tools/||g" > "$OUT/caller.koruby"
if diff -u "$OUT/caller.cruby" "$OUT/caller.koruby"; then
  echo "caller(0): identical to CRuby ($(ruby -v | cut -d' ' -f1-2))"
else
  echo "caller(0): DIFFERS"
  exit 1
fi
