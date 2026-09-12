#!/usr/bin/env bash
# Instruction-count A/B harness for the frame-layout work.
#   tools/bt_instr.sh <tag>
# Bakes each micro-bench AOT into a private code store, then counts
# instructions/cycles with perf.  Output: <tag>.instr.txt in the out dir.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
TAG=${1:?usage: bt_instr.sh <tag>}
OUT=${OUT:-/tmp/claude-1000/-home-ko1-ruby-astro/0a48b788-4b01-46f0-a6f8-490a93bf91eb/scratchpad/bt}
mkdir -p "$OUT"
cd "$HERE"
BENCHES="fib method_call ackermann ivar"
: > "$OUT/$TAG.instr.txt"
for b in $BENCHES; do
  f="$HERE/../rubyharness/bench/$b.rb"
  rm -rf code_store
  CCACHE_DISABLE=1 ./koruby_precise --aot-compile "$f" >/dev/null 2>"$OUT/$TAG.$b.compile.log" || { echo "$b COMPILE-FAIL" >> "$OUT/$TAG.instr.txt"; continue; }
  txt=$(size -A code_store/op/*.o 2>/dev/null | awk '$1==".text"{s+=$2} END{print s+0}')
  perf stat -r 5 -e instructions,cycles -x, -o "$OUT/$TAG.$b.perf" ./koruby_precise --compiled-only "$f" >/dev/null 2>&1
  ins=$(awk -F, '$3=="instructions"{print $1}' "$OUT/$TAG.$b.perf")
  cyc=$(awk -F, '$3=="cycles"{print $1}' "$OUT/$TAG.$b.perf")
  echo "$b instructions=$ins cycles=$cyc text=$txt" >> "$OUT/$TAG.instr.txt"
done
cat "$OUT/$TAG.instr.txt"
