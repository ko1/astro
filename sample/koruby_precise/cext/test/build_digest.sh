#!/bin/bash
# Build CRuby's ext/digest unchanged against koruby's cext/include (no mkmf yet:
# the defines are what mkmf's extconf.h / Makefile give on x86_64 Linux).
#   bash cext/test/build_digest.sh <cruby ext/digest dir> <out dir>
# Out: <out>/digest.so, <out>/digest/{md5,sha1,sha2,rmd160,bubblebabble,crc32,blake3}.so,
# and <out>/lib (ext/digest/lib + the per-algorithm lib/: digest.rb, digest/{loader,version,sha2}.rb ...).
set -eu
SRC=${1:-$HOME/ruby/src/master/ext/digest}
OUT=${2:-$(dirname "$0")/build/digest}
KDIR=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$OUT/digest" "$OUT/obj"
CC=${CC:-cc}
CF="-O2 -g -fPIC -Wall -Wno-unused-parameter -Wno-sign-compare -I$KDIR/cext/include -I$SRC -DHAVE_SYS_CDEFS_H=1"
so() {   # so <out.so> <extra cflags> <src...>
    local o=$1 x=$2; shift 2
    local objs=()
    for s in "$@"; do
        local ob="$OUT/obj/$(echo "${s#$SRC/}" | tr / _).o"
        $CC $CF $x -c "$s" -o "$ob"
        objs+=("$ob")
    done
    $CC -shared -o "$o" "${objs[@]}"
}
so "$OUT/digest.so" "" "$SRC/digest.c"
so "$OUT/digest/md5.so" "" "$SRC/md5/md5.c" "$SRC/md5/md5init.c"
so "$OUT/digest/sha1.so" "" "$SRC/sha1/sha1.c" "$SRC/sha1/sha1init.c"
so "$OUT/digest/sha2.so" "-DHAVE_TYPE_U_INT8_T=1" "$SRC/sha2/sha2.c" "$SRC/sha2/sha2init.c"
so "$OUT/digest/rmd160.so" "" "$SRC/rmd160/rmd160.c" "$SRC/rmd160/rmd160init.c"
so "$OUT/digest/bubblebabble.so" "" "$SRC/bubblebabble/bubblebabble.c"
[ -d "$SRC/crc32" ] && so "$OUT/digest/crc32.so" "" "$SRC"/crc32/*.c
if [ -d "$SRC/blake3" ]; then
    B=$SRC/blake3
    bo=()
    for f in blake3init blake3 blake3_dispatch blake3_portable; do $CC $CF -c "$B/$f.c" -o "$OUT/obj/$f.o"; bo+=("$OUT/obj/$f.o"); done
    $CC $CF -msse2 -c "$B/blake3_sse2.c" -o "$OUT/obj/blake3_sse2.o"; bo+=("$OUT/obj/blake3_sse2.o")
    $CC $CF -msse4.1 -c "$B/blake3_sse41.c" -o "$OUT/obj/blake3_sse41.o"; bo+=("$OUT/obj/blake3_sse41.o")
    $CC $CF -mavx2 -c "$B/blake3_avx2.c" -o "$OUT/obj/blake3_avx2.o"; bo+=("$OUT/obj/blake3_avx2.o")
    $CC $CF -mavx512f -mavx512vl -c "$B/blake3_avx512.c" -o "$OUT/obj/blake3_avx512.o"; bo+=("$OUT/obj/blake3_avx512.o")
    $CC -shared -o "$OUT/digest/blake3.so" "${bo[@]}"
fi
rm -rf "$OUT/lib"; cp -r "$SRC/lib" "$OUT/lib"
for d in "$SRC"/*/lib; do cp -r "$d"/. "$OUT/lib/digest/"; done   # sha2/lib: Digest::SHA2 (Ruby) + its loader
# every symbol the extensions need must come from koruby (or libc / each other)
nm -D --defined-only "$KDIR/koruby_precise" | awk '{print $3}' | sort -u > "$OUT/obj/provided.txt"
for f in "$OUT"/digest.so "$OUT"/digest/*.so; do nm -D --defined-only "$f" | awk '{print $3}'; done | sort -u >> "$OUT/obj/provided.txt"
missing=$(for f in "$OUT"/digest.so "$OUT"/digest/*.so; do nm -Du "$f" | awk '{print $2}'; done | sort -u | grep -E '^(rb_|ruby_)' | grep -vxF -f "$OUT/obj/provided.txt" || true)
if [ -n "$missing" ]; then echo "missing C API: $missing"; exit 1; fi
echo "built $(ls "$OUT"/digest.so "$OUT"/digest/*.so | wc -l) extensions in $OUT"
