#!/usr/bin/env bash
# Build koruby's install root (root/) from a CRuby install of the same version, so
# that koruby alone resolves `require` the way CRuby does:
#   root/lib/ruby/<ver>/            CRuby's pure-Ruby stdlib, rubygems and bundler
#                                   (koruby's own lib/ stays in front of it)
#   root/lib/ruby/gems/<ver>/       Gem.default_dir: CRuby's default-gem specs, and the
#                                   bundled gems installed by koruby's own `gem install`
#   root/bin/                       gem / bundle / rake … with a koruby shebang
# RbConfig's prefix defaults to this root when it exists (prelude/system.rb).
#   bash tools/make_root.sh [CRUBY]      (CRUBY: a ruby binary, default ~/ruby/install/master/bin/ruby)
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
echo "make_root: stdlib entries=$n default specs=$(ls "$GEMS/specifications/default" | wc -l)"

# bundled gems: installed with koruby's own rubygems from CRuby's cached .gem files
BUNDLED=$("$CRUBY" -e 'Gem::Specification.each { |s| puts s.cache_file if !s.default_gem? && s.base_dir == Gem.default_dir && File.exist?(s.cache_file) }')
# shellcheck disable=SC2086
env -u GEM_HOME -u GEM_PATH -u RUBYLIB -u KORUBY_RUBYLIB -u KORUBY_PREFIX \
  "$K" "$ROOT/bin/gem" install --local --no-document --ignore-dependencies $BUNDLED
echo "make_root: bundled gems=$(echo "$BUNDLED" | wc -l) -> $GEMS"
touch "$ROOT/.complete"
