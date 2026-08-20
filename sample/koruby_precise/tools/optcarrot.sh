#!/usr/bin/env bash
# Build a require-free optcarrot bundle (koruby_precise has no require/ARGV) and
# run it.  Usage: tools/optcarrot.sh [FRAMES] [KORUBY_BIN]
#   OPTC_MODE=plain (default) — run the pure tree-walker (--plain, ignores any
#                               code store), the reproducible interpreter number.
#   OPTC_MODE=aot             — bake the bundle (--aot-compile) then run it
#                               --compiled-only (poisons unswapped bodies), the
#                               AOT FPS number.
#   OPTC_MODE=build           — only write the bundle, print its path, don't run
#                               (used by tools/optcarrot_bench.sh).
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
OPT="$HERE/../abruby/benchmark/optcarrot"
FRAMES=${1:-30}
BIN=${2:-$HERE/koruby_precise}
case "$BIN" in /*) ;; *) BIN="$PWD/$BIN";; esac   # absolutize: we cd into $OPT below
MODE=${OPTC_MODE:-plain}
BUNDLE=/tmp/optc_bundle.rb
{
  # File shim: koruby_precise has only the __binread C primitive; layer File's
  # class methods on top in Ruby (basename/extname are pure string ops).
  cat <<'SHIM'
class File
  def self.binread(path) = __binread(path)
  def self.read(path) = __binread(path)
  def self.basename(p) = (p.split("/").last || p)
  def self.extname(p)
    b = p.split("/").last || ""
    i = b.rindex(".")
    (i && i > 0) ? b[i..-1] : ""
  end
end
module Process
  def self.clock_gettime(clk) = __clock_gettime   # CLOCK_MONOTONIC comes from the prelude
end
SHIM
  # require-order: opt.rb (CodeOptimizationHelper) before cpu/ppu; mappers after
  # rom.rb (loaded at runtime by rom).  :none driver pulls no driver/*.
  for f in lib/optcarrot.rb lib/optcarrot/opt.rb lib/optcarrot/nes.rb \
           lib/optcarrot/rom.rb lib/optcarrot/pad.rb lib/optcarrot/cpu.rb \
           lib/optcarrot/apu.rb lib/optcarrot/ppu.rb lib/optcarrot/palette.rb \
           lib/optcarrot/driver.rb lib/optcarrot/config.rb \
           lib/optcarrot/mapper/mmc1.rb lib/optcarrot/mapper/uxrom.rb \
           lib/optcarrot/mapper/cnrom.rb lib/optcarrot/mapper/mmc3.rb; do
    grep -vE '^[[:space:]]*require_relative|^[[:space:]]*require ' "$OPT/$f"; echo
  done
  echo "Optcarrot::NES.new([\"-b\", \"--frames\", \"$FRAMES\", \"examples/Lan_Master.nes\"]).run"
} > "$BUNDLE"
if [ "$MODE" = build ]; then echo "$BUNDLE"; exit 0; fi
cd "$OPT" || exit 1
if [ "$MODE" = aot ]; then
  rm -rf code_store
  CCACHE_DISABLE=1 "$BIN" --aot-compile "$BUNDLE" || exit 1   # bake the code store (no run)
  exec "$BIN" --compiled-only "$BUNDLE"                        # run only baked SDs
fi
exec "$BIN" --plain "$BUNDLE"                                  # pure tree-walker
