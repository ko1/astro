#!/bin/bash
# CRuby's ext/digest built for koruby (build_digest.sh), then CRuby's own
# test/digest run by koruby.  KORUBY_CEXT=1 makes the extension win over
# koruby's pure-Ruby lib/digest stand-ins; KORUBY_RUBYLIB carries the load path
# into the assert_separately children.  Prints one summary line per test file;
# exit status 0 only if every file reports 0 failures and 0 errors.
#   bash cext/test/digest.sh [cruby source tree]
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
KDIR=$(cd "$HERE/../.." && pwd)
CSRC=${1:-$HOME/ruby/src/master}
OUT=$HERE/build/digest
bash "$HERE/build_digest.sh" "$CSRC/ext/digest" "$OUT" || exit 1
st=0
cd "$CSRC" || exit 1
for f in test/digest/test_digest.rb test/digest/test_digest_extend.rb test/digest/test_ractor.rb; do
    line=$(KORUBY_CEXT=1 KORUBY_RUBYLIB="$OUT/lib:$OUT" timeout 600 "$KDIR/koruby_precise" -I tool/lib "$f" 2>&1 | grep -E '^[0-9]+ tests,')
    echo "$f: ${line:-NO SUMMARY}"
    case "$line" in *" 0 failures, 0 errors"*) ;; *) st=1 ;; esac
done
exit $st
