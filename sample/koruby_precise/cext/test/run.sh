#!/bin/bash
# C-API conformance: build api_test.c for CRuby and for koruby, run api_test.rb
# under each, diff; then the koruby run again under STRESS+PURGE (a moving
# collection at every allocation).  Exit status 0 only if all three agree.
#   bash cext/test/run.sh [cruby]      (default: ~/ruby/install/master/bin/ruby)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
KDIR=$(cd "$HERE/../.." && pwd)
RUBY=${1:-$HOME/ruby/install/master/bin/ruby}
OUT=${CEXT_TEST_OUT:-$HERE/build}
mkdir -p "$OUT/cruby" "$OUT/koruby"
CFLAGS="-O1 -g -fPIC -shared -Wall -Wno-unused-parameter -Wno-deprecated-declarations"

HDR=$("$RUBY" -e 'print RbConfig::CONFIG["rubyhdrdir"]')
AHDR=$("$RUBY" -e 'print RbConfig::CONFIG["rubyarchhdrdir"]')
cc $CFLAGS -I"$AHDR" -I"$HDR" "$HERE/api_test.c" -o "$OUT/cruby/cext_api_test.so" || exit 1
cc $CFLAGS -I"$KDIR/cext/include" "$HERE/api_test.c" -o "$OUT/koruby/cext_api_test.so" || exit 1

"$RUBY" "$HERE/api_test.rb" "$OUT/cruby" > "$OUT/cruby.out" 2>&1
"$KDIR/koruby_precise" "$HERE/api_test.rb" "$OUT/koruby" > "$OUT/koruby.out" 2>&1
BARUBY_GC_STRESS=1 BARUBY_GC_PURGE=1 "$KDIR/koruby_precise" "$HERE/api_test.rb" "$OUT/koruby" > "$OUT/koruby_stress.out" 2>&1
st=0
norm() { sed -E "s/0x[0-9a-f]{8,16}/0xXXXX/g" "$1"; }
diff <(norm "$OUT/cruby.out") <(norm "$OUT/koruby.out") > "$OUT/diff.txt" || st=1
diff <(norm "$OUT/cruby.out") <(grep -v '^\[GC\]\|^\[aro_gc' "$OUT/koruby_stress.out" | norm /dev/stdin) > "$OUT/diff_stress.txt" || st=1
# a CRuby-built extension must stay "not found" (it would run CRuby's ABI and drag in libruby)
refuse=$("$KDIR/koruby_precise" -e 'begin; require ARGV[0]; puts "LOADED"; rescue LoadError => e; puts "refused"; end' "$OUT/cruby/cext_api_test.so" 2>&1)
[ "$refuse" = refused ] || { echo "CRuby-built .so was not refused: $refuse"; st=1; }
n=$(grep -c ': ' "$OUT/cruby.out")
echo "cases: $n  diff: $(grep -c '^>' "$OUT/diff.txt")  diff(STRESS+PURGE): $(grep -c '^>' "$OUT/diff_stress.txt")"
[ $st = 0 ] && echo OK || { echo "see $OUT/diff.txt $OUT/diff_stress.txt"; }
exit $st
