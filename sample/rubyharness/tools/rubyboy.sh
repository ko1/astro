#!/bin/sh
# rubyboy.sh — pure-Ruby Game Boy emulator headless benchmark for the rubyharness
# model.  Runs sacckey/rubyboy's EmulatorHeadless for a fixed number of frames on
# a fixed ROM (tobu.gb) and prints a framebuffer checksum (deterministic — CRuby
# and an ASTro sample must agree).  The harness/user times wall clock externally;
# correctness = matching checksum (like optcarrot / doom).
#
# The engine is bundled into one require-free file (requires stripped, all
# headless modules concatenated — module nesting is preserved per file and every
# cross-reference is inside a method, so concat order is free).  Bundling lets
# --aot-compile bake the hot cpu/ppu code, not just a require'd shim.
#
# Env:
#   RB_MODE   plain (default) — run the tree-walker (INTERP --plain)
#             aot             — bake (--aot-compile) then run --compiled-only
#             cruby           — run with $RUBY --yjit-disable (oracle)
#             cruby-yjit      — run with $RUBY --yjit
#             build           — only write the bundle, print its path, exit
#   INTERP    the sample binary            (default ./koruby_precise)
#   RUBY      reference ruby               (default ruby)
#   FRAMES    frames to render             (default 60)
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../apps/rubyboy"
MODE=${RB_MODE:-plain}
INTERP=${INTERP:-./koruby_precise}
RUBY=${RUBY:-ruby}
FRAMES=${FRAMES:-60}
BUNDLE=${RUBYBOY_BUNDLE:-/tmp/rubyboy_bundle.rb}

[ -f "$APP/lib/roms/tobu.gb" ] || sh "$HERE/rubyboy_setup.sh"

# Absolutize INTERP (we cd into $APP for the AOT code_store below).
case "$INTERP" in /*|*" "*) ;; ./*) INTERP="$(cd "$(dirname "$INTERP")" && pwd)/$(basename "$INTERP")";; esac

L="$APP/lib/rubyboy"

# --- build the bundle: headless engine (requires stripped) + driver body ---
{
  printf 'FRAMES = %s\nROM_PATH = "%s/lib/roms/tobu.gb"\n' "$FRAMES" "$APP"
  for f in registers.rb rom.rb ram.rb interrupt.rb timer.rb joypad.rb ppu.rb \
           apu_channels/channel1.rb apu_channels/channel2.rb \
           apu_channels/channel3.rb apu_channels/channel4.rb apu.rb \
           cartridge/nombc.rb cartridge/mbc1.rb cartridge/factory.rb \
           bus.rb cpu.rb emulator_headless.rb; do
    grep -vE "^[[:space:]]*require(_relative)? " "$L/$f"; echo
  done
  cat <<'BODY'
emu = Rubyboy::EmulatorHeadless.new(ROM_PATH)
FRAMES.times { emu.step }
buf = emu.instance_variable_get(:@ppu).buffer
# FNV-1a over the framebuffer (deterministic; CRuby and the sample must agree).
h = 14695981039346656037
buf.each do |px|
  v = px.is_a?(Array) ? (px[0] << 16 | px[1] << 8 | px[2]) : px.to_i
  4.times { |i| h = ((h ^ ((v >> (i * 8)) & 0xff)) * 1099511628211) & 0xffffffffffffffff }
end
puts "frames: #{FRAMES}"
puts "pixels: #{buf.size}"
puts "checksum: #{h}"
BODY
} > "$BUNDLE"

case "$MODE" in
  build)      echo "$BUNDLE"; exit 0 ;;
  cruby)      exec "$RUBY" --yjit-disable "$BUNDLE" ;;
  cruby-yjit) exec "$RUBY" --yjit "$BUNDLE" ;;
  aot)
    cd "$APP"; rm -rf code_store
    CCACHE_DISABLE=1 "$INTERP" --aot-compile "$BUNDLE" >/dev/null 2>&1 || { echo "aot-compile failed"; exit 1; }
    exec "$INTERP" --compiled-only "$BUNDLE" ;;
  plain|*)    exec "$INTERP" --plain "$BUNDLE" ;;
esac
