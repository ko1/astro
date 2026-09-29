#!/bin/bash
# Build CRuby's ext/openssl unchanged against koruby's cext/include.  The HAVE_*
# defines come from the extconf.h mkmf wrote in a built CRuby tree (same OpenSSL).
#   bash cext/build_openssl.sh <cruby ext/openssl dir (built)> <out dir>
# Out: <out>/openssl.ko.so and <out>/lib (ext/openssl/lib: openssl.rb, openssl/*.rb).
set -eu
SRC=${1:-$HOME/ruby/src/master/ext/openssl}
OUT=${2:-$(dirname "$0")/test/build/openssl}
KDIR=$(cd "$(dirname "$0")/.." && pwd)
[ -f "$SRC/extconf.h" ] || { echo "no $SRC/extconf.h: run CRuby's build (mkmf) there first"; exit 1; }
mkdir -p "$OUT/obj"
CF="-O2 -g -fPIC -Wall -Wno-unused-parameter -Wno-sign-compare -Wno-deprecated-declarations -I$KDIR/cext/include -I$SRC -DRUBY_EXTCONF_H=\"extconf.h\""
objs=()
for s in "$SRC"/*.c; do
    o="$OUT/obj/$(basename "${s%.c}").o"
    ${CC:-cc} $CF -c "$s" -o "$o"
    objs+=("$o")
done
${CC:-cc} -shared -o "$OUT/openssl.ko.so" "${objs[@]}" -lssl -lcrypto
rm -rf "$OUT/lib"; cp -r "$SRC/lib" "$OUT/lib"
missing=$(nm -Du "$OUT/openssl.ko.so" | awk '{print $2}' | grep -E '^(rb_|ruby_)' | \
          grep -vxF -f <(nm -D --defined-only "$KDIR/koruby_precise" | awk '{print $3}') || true)
if [ -n "$missing" ]; then echo "missing C API: $missing"; exit 1; fi
echo "built $OUT/openssl.ko.so"
