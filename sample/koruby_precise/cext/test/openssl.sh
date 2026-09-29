#!/bin/bash
# CRuby's ext/openssl built for koruby (build_openssl.sh), then CRuby's own
# test/openssl/test_*.rb one file at a time under CRuby and under koruby.
# One TSV row per file: file  cruby_summary  koruby_summary  koruby_first_error.
#   bash cext/test/openssl.sh [cruby source tree] [out.tsv]
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
KDIR=$(cd "$HERE/../.." && pwd)
CSRC=${1:-$HOME/ruby/src/master}
OUTTSV=${2:-$HERE/build/openssl.tsv}
R=${CEXT_RUBY:-$HOME/ruby/install/master/bin/ruby}
OUT=$HERE/build/openssl
bash "$HERE/../build_openssl.sh" "$CSRC/ext/openssl" "$OUT" > /dev/null || exit 1
DG=$HERE/build/digest   # OpenSSL::Digest < ::Digest::Class: the digest extension too
bash "$HERE/../build_digest.sh" "$CSRC/ext/digest" "$DG" > /dev/null || exit 1
sum() { grep -oE '^[0-9]+ tests, [0-9]+ assertions, [0-9]+ failures, [0-9]+ errors, [0-9]+ (skips|pendings)' <<<"$1" | tail -1; }
cd "$CSRC" || exit 1
: > "$OUTTSV"
for f in test/openssl/test_*.rb; do
    co=$(timeout 300 "$R" -I tool/lib "$f" 2>&1)
    ko=$(KORUBY_RUBYLIB="$OUT/lib:$OUT:$DG/lib:$DG" timeout 600 "$KDIR/koruby_precise" -I tool/lib "$f" 2>&1)
    e=$(grep -m1 -oE '[A-Za-z:]*(Error|Exception)[^\n]{0,80}' <<<"$ko" | head -1)
    printf '%s\t%s\t%s\t%s\n' "$(basename "$f")" "$(sum "$co")" "$(sum "$ko")" "$e" >> "$OUTTSV"
done
awk -F'\t' '{ n++; if ($2 == $3) same++ } END { printf "files %d, same summary %d\n", n, same }' "$OUTTSV"
