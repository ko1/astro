#!/usr/bin/env bash
# Build koruby's install root (root/) from a CRuby install of the same version, so
# that koruby alone resolves `require` the way CRuby does:
#   root/lib/ruby/<ver>/            CRuby's pure-Ruby stdlib, rubygems and bundler
#                                   (koruby's own lib/ stays in front of it)
#   root/lib/ruby/gems/<ver>/       Gem.default_dir: CRuby's default-gem specs, and the
#                                   bundled gems installed by koruby's own `gem install`
#   root/bin/                       gem / bundle / rake … with a koruby shebang
# RbConfig's prefix defaults to this root when it exists (prelude/system.rb).
#   bash tools/make_root.sh [CRUBY]      (CRUBY: a ruby binary, default ~/ruby/install/master/bin/ruby;
#                                         CRUBY_SRC: its built source tree, default ~/ruby/src/master)
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
K=$HERE/koruby_precise
CRUBY=${1:-$HOME/ruby/install/master/bin/ruby}
[ -x "$K" ] || { echo "make_root: build koruby_precise first" >&2; exit 1; }
SRC_LIB=$("$CRUBY" -e 'print RbConfig::CONFIG["rubylibdir"]')
SRC_GEMS=$("$CRUBY" -e 'print Gem.default_dir')
SRC_BIN=$("$CRUBY" -e 'print RbConfig::CONFIG["bindir"]')
VER=$("$K" -e 'print RUBY_VERSION')
ROOT=$HERE/root
LIB=$ROOT/lib/ruby/$VER
GEMS=$ROOT/lib/ruby/gems/$VER
rm -rf "$ROOT.tmp"; mkdir -p "$ROOT.tmp/lib/ruby/$VER" "$ROOT.tmp/lib/ruby/gems/$VER/specifications/default" "$ROOT.tmp/bin"

# stdlib: everything except what koruby's lib/ implements itself (a CRuby file
# of the same name would wrap a C extension koruby does not have, or mix with
# koruby's version), the C-extension arch dir, and openssl/ (it reopens classes
# openssl.so defines).  rubygems/ is taken even though koruby's lib/ has a
# rubygems/defaults/ of its own: that dir only adds the operating_system hook.
n=0
for p in "$SRC_LIB"/*; do
  b=$(basename "$p")
  case $b in
    rubygems|rubygems.rb) ;;
    openssl|x86_64-linux|*-linux|*-darwin*) continue ;;
    *) [ -e "$HERE/lib/$b" ] && continue ;;
  esac
  cp -a "$p" "$ROOT.tmp/lib/ruby/$VER/"; n=$((n+1))
done
# default gems: their specs (the code is in the stdlib above or koruby's lib/)
cp -a "$SRC_GEMS/specifications/default/." "$ROOT.tmp/lib/ruby/gems/$VER/specifications/default/"
# ... and the gem dirs some of them keep executables in (bundler's exe/bundle, erb's libexec)
mkdir -p "$ROOT.tmp/lib/ruby/gems/$VER/gems"
for s in "$SRC_GEMS"/specifications/default/*.gemspec; do
  d=$SRC_GEMS/gems/$(basename "$s" .gemspec)
  [ -d "$d" ] && cp -a "$d" "$ROOT.tmp/lib/ruby/gems/$VER/gems/"
done
cp -a "$SRC_BIN/gem" "$SRC_BIN/bundle" "$SRC_BIN/bundler" "$ROOT.tmp/bin/" 2>/dev/null || true
for f in "$ROOT.tmp"/bin/*; do sed -i "1s|^#!.*|#!$K|" "$f"; done
rm -rf "$ROOT"; mv "$ROOT.tmp" "$ROOT"

# mkmf: RbConfig::MAKEFILE_CONFIG (CRuby's, with koruby's layout and DLEXT kompat.so;
# prelude autoloads it) and the compatibility layer's headers at rubyhdrdir, so an
# extconf.rb + make builds foo.kompat.so for koruby
ARCHNAME=$("$K" -e 'print RbConfig::CONFIG["arch"]')
mkdir -p "$LIB/$ARCHNAME" "$ROOT/include/ruby-$VER/$ARCHNAME"
"$CRUBY" "$HERE/tools/make_mkmf_config.rb" > "$LIB/$ARCHNAME/koruby_makefile_config.rb"
cp -r "$HERE/cext/include/." "$ROOT/include/ruby-$VER/"
# paths mkmf's Makefiles name ($(arch_hdrdir)/ruby/config.h, -I$(hdrdir)/ruby/backward)
mkdir -p "$ROOT/include/ruby-$VER/$ARCHNAME/ruby" "$ROOT/include/ruby-$VER/ruby/backward"
printf '/* koruby: folded into <ruby.h> */\n#include <ruby.h>\n' > "$ROOT/include/ruby-$VER/$ARCHNAME/ruby/config.h"
# every other header of CRuby's tree as a stub: an extension's `depend` names them as make prerequisites
CRUBY_HDR=$("$CRUBY" -e 'print RbConfig::CONFIG["rubyhdrdir"]')
(cd "$CRUBY_HDR" && find . -name '*.h' ! -path "./$ARCHNAME/*") | while read -r h; do
    [ -e "$ROOT/include/ruby-$VER/$h" ] && continue
    mkdir -p "$(dirname "$ROOT/include/ruby-$VER/$h")"
    printf '/* koruby: folded into <ruby.h> */\n#include <ruby.h>\n' > "$ROOT/include/ruby-$VER/$h"
done
# the platform's HAVE_* (libc features: localtime_r ...), SIZEOF_* and int128_t from CRuby's config.h; ruby.h includes it
CRUBY_ARCHHDR=$("$CRUBY" -e 'print RbConfig::CONFIG["rubyarchhdrdir"]')
awk '/^#define ((HAVE_|SIZEOF_)[A-Z0-9_]+|u?int128_t) / { print "#ifndef " $2; print; print "#endif" }' "$CRUBY_ARCHHDR/ruby/config.h" \
    > "$ROOT/include/ruby-$VER/$ARCHNAME/ruby/koruby_platform.h"
# libkoruby.so: empty stand-ins named like every rb_* / ruby_* the koruby binary exports.
# mkmf links its have_func("rb_...") probes and extensions against -lkoruby; at run time
# the binary's own definitions come first in symbol lookup, so these are never called.
mkdir -p "$ROOT/lib"
nm -D --defined-only -S "$K" | "$CRUBY" -e '
  puts "/* link-time stand-ins for the C API of the koruby binary (tools/make_root.sh) */"
  STDIN.each_line do |l|
    f = l.split
    next unless f.size == 4 && f[3] =~ /\A(rb_|ruby_)/
    puts f[2] =~ /[BbDd]/ ? "char #{f[3]}[#{f[1].to_i(16)}];" : "void #{f[3]}(void) {}"
  end' > "$ROOT/lib/libkoruby.stub.c"
cc -shared -fPIC -w -o "$ROOT/lib/libkoruby.so" "$ROOT/lib/libkoruby.stub.c"
echo "make_root: stdlib entries=$n default specs=$(ls "$GEMS/specifications/default" | wc -l)"

# C extensions built for koruby (cext/): CRuby's ext/digest and ext/openssl, unmodified,
# into vendor_ruby — in front of koruby's lib/, whose digest.rb / openssl.rb are the
# pure-Ruby stand-ins for a koruby without them.  CRUBY_SRC is a BUILT CRuby source tree
# (openssl's extconf.h is mkmf's output there); without it this step is skipped.
CRUBY_SRC=${CRUBY_SRC:-$HOME/ruby/src/master}
ARCH=$("$K" -e 'print RbConfig::CONFIG["arch"]')
VEND=$ROOT/lib/ruby/vendor_ruby/$VER
if [ -f "$CRUBY_SRC/ext/openssl/extconf.h" ] && [ -d "$CRUBY_SRC/ext/digest" ]; then
  EXT=$ROOT.ext; rm -rf "$EXT"
  bash "$HERE/cext/build_digest.sh" "$CRUBY_SRC/ext/digest" "$EXT/digest" > /dev/null
  bash "$HERE/cext/build_openssl.sh" "$CRUBY_SRC/ext/openssl" "$EXT/openssl" > /dev/null
  mkdir -p "$VEND/$ARCH/digest"
  cp -r "$EXT/digest/lib/." "$EXT/openssl/lib/." "$VEND/"
  cp "$EXT/digest/digest.kompat.so" "$EXT/openssl/openssl.kompat.so" "$VEND/$ARCH/"
  cp "$EXT"/digest/digest/*.kompat.so "$VEND/$ARCH/digest/"
  rm -rf "$EXT"
  echo "make_root: C extensions digest, openssl -> $VEND"
else
  echo "make_root: no built CRuby tree at $CRUBY_SRC (ext/openssl/extconf.h): C extensions skipped, lib/ stand-ins apply"
fi

# bundled gems: installed with koruby's own rubygems from CRuby's cached .gem files
BUNDLED=$("$CRUBY" -e 'Gem::Specification.each { |s| puts s.cache_file if !s.default_gem? && s.base_dir == Gem.default_dir && File.exist?(s.cache_file) }')
# shellcheck disable=SC2086
env -u GEM_HOME -u GEM_PATH -u RUBYLIB -u KORUBY_RUBYLIB -u KORUBY_PREFIX \
  "$K" "$ROOT/bin/gem" install --local --no-document --ignore-dependencies $BUNDLED
echo "make_root: bundled gems=$(echo "$BUNDLED" | wc -l) -> $GEMS"
touch "$ROOT/.complete"
