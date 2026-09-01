#!/bin/sh
# Run corpus files under GC STRESS+PURGE and diff against CRuby.  Usage:
#   tools/stress_diff.sh <name>...     (names are ../rubyharness/t/hand/<name>.rb)
cd "$(dirname "$0")/.." || exit 1
rc=0
for f in "$@"; do
  src="../rubyharness/t/hand/$f.rb"
  BARUBY_GC_STRESS=1 BARUBY_GC_PURGE=1 ./koruby_precise "$src" 2>&1 | grep -v 'STRESS (GC every alloc)' > "$TMPDIR/sd_a.txt"
  ruby "$src" > "$TMPDIR/sd_b.txt" 2>&1
  if diff -q "$TMPDIR/sd_a.txt" "$TMPDIR/sd_b.txt" >/dev/null; then
    echo "OK   $f"
  else
    echo "DIFF $f"; diff "$TMPDIR/sd_a.txt" "$TMPDIR/sd_b.txt" | head -10; rc=1
  fi
done
exit $rc
