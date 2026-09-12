#!/usr/bin/env bash
# Wider instruction/cycle A/B (closure- and block-heavy shapes).
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
TAG=${1:?usage: bt_instr2.sh <tag>}
OUT=${OUT:-/tmp/claude-1000/-home-ko1-ruby-astro/0a48b788-4b01-46f0-a6f8-490a93bf91eb/scratchpad/bt}
mkdir -p "$OUT"
cd "$HERE"
BENCHES=${BENCHES:-"closures block iterators object methodchain block_yield_kernel"}
: > "$OUT/$TAG.instr2.txt"
for b in $BENCHES; do
  f="$HERE/../rubyharness/bench/$b.rb"
  [ -f "$f" ] || { echo "$b MISSING" >> "$OUT/$TAG.instr2.txt"; continue; }
  rm -rf code_store
  CCACHE_DISABLE=1 ./koruby_precise --aot-compile "$f" >/dev/null 2>&1 || { echo "$b COMPILE-FAIL" >> "$OUT/$TAG.instr2.txt"; continue; }
  perf stat -r 5 -e instructions,cycles,L1-dcache-load-misses -x, -o "$OUT/$TAG.$b.perf2" ./koruby_precise --compiled-only "$f" >/dev/null 2>&1
  ins=$(awk -F, '$3=="instructions"{print $1}' "$OUT/$TAG.$b.perf2")
  cyc=$(awk -F, '$3=="cycles"{print $1}' "$OUT/$TAG.$b.perf2")
  dm=$(awk -F, '$3=="L1-dcache-load-misses"{print $1}' "$OUT/$TAG.$b.perf2")
  echo "$b instructions=$ins cycles=$cyc dmiss=$dm" >> "$OUT/$TAG.instr2.txt"
done
cat "$OUT/$TAG.instr2.txt"
