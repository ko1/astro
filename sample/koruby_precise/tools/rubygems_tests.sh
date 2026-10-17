#!/usr/bin/env bash
# Run CRuby's test/rubygems file by file under koruby and CRuby; print per-file "tests/failures/errors/skips".
# usage: tools/rubygems_tests.sh OUTDIR [JOBS]   (CRUBY_SRC, CRUBY override the master tree / ruby)
set -u
OUT=${1:?outdir}; J=${2:-8}
KO=$(cd "$(dirname "$0")/.." && pwd)/koruby_precise
SRC=${CRUBY_SRC:-$HOME/ruby/src/master}
CR=${CRUBY:-$HOME/ruby/install/master/bin/ruby}
mkdir -p "$OUT"
one() {
  local f=$1 b; b=$(basename "$f" .rb)
  for k in ko cr; do
    local r=$KO; [ $k = cr ] && r=$CR
    (cd "$SRC" && timeout -s KILL 300 "$r" -I tool/lib -I test/lib tool/test/runner.rb "$f" > "$OUT/$b.$k" 2>&1); echo "rc=$?" >> "$OUT/$b.$k"
  done
}
export -f one; export OUT KO SRC CR
ls "$SRC"/test/rubygems/test_*.rb | sed "s|$SRC/||" | xargs -P "$J" -I{} bash -c 'one {}'
for f in "$OUT"/*.ko; do
  b=$(basename "$f" .ko)
  s() { grep -E '^[0-9]+ tests, ' "$1" | tail -1 | sed -E 's/([0-9]+) tests, [0-9]+ assertions, ([0-9]+) failures, ([0-9]+) errors, ([0-9]+) skips.*/\1 \2 \3 \4/'; }
  k=$(s "$f"); c=$(s "$OUT/$b.cr")
  echo "$b ko=[${k:-NONE $(tail -1 "$f")}] cr=[${c:-NONE $(tail -1 "$OUT/$b.cr")}]"
done > "$OUT/summary.txt"
echo DONE
