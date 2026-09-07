#!/bin/sh
# rubyboy_web.sh — build the interactive Game Boy bundle for the browser (wasm).
#
# rubyboy already has an EmulatorWasm: step(direction_key, action_key) runs one
# frame and returns the 160x144 framebuffer.  All this adds is the same one byte
# in / one frame out protocol the DOOM page uses, so the Worker side is shared.
#
# Env:
#   ROM   path the bundle opens the ROM at (default /gb/rom.gb)
#   OUT   bundle path (default /tmp/rubyboy_web.rb)
set -e

HERE=$(cd "$(dirname "$0")/.." && pwd)
APP="$HERE/../rubyharness/apps/rubyboy"
ROM=${ROM:-/gb/rom.gb}
OUT=${OUT:-/tmp/rubyboy_web.rb}

{
  # koruby has only the __binread primitive; CRuby already has File.binread,
  # so defining it unconditionally would break ruby.wasm.
  cat <<'SHIM'
unless File.respond_to?(:binread)
  class File
    def self.binread(path) = __binread(path)
    def self.read(path) = __binread(path)
  end
end
SHIM
  # Dependency order; SDL / raylib / FFI drivers are left out on purpose.
  for f in lib/rubyboy/version.rb \
           lib/rubyboy/interrupt.rb lib/rubyboy/ram.rb lib/rubyboy/rom.rb \
           lib/rubyboy/registers.rb lib/rubyboy/timer.rb lib/rubyboy/joypad.rb \
           lib/rubyboy/lcd.rb lib/rubyboy/ppu.rb \
           lib/rubyboy/apu_channels/*.rb lib/rubyboy/apu.rb \
           lib/rubyboy/cartridge/*.rb \
           lib/rubyboy/cpu.rb lib/rubyboy/bus.rb; do
    for g in $APP/$f; do
      [ -f "$g" ] || continue
      grep -vE '^[[:space:]]*require_relative|^[[:space:]]*require ' "$g"; echo
    done
  done
  cat <<BODY
ROM_PATH = "$ROM"
BODY
  cat <<'BODY'
# --- interactive driver ------------------------------------------------------
# Protocol, one byte in / one frame out:
#   in   bit 0..3 = Right Left Up Down, bit 4..7 = A B Select Start.
#        The Game Boy joypad is active-low, so a set bit means NOT pressed and
#        the idle byte is 0xff... which would collide with quit, so the host
#        sends the pressed-high form and this side inverts.  0xfe = quit.
#   out  160 * 144 * 4 bytes, 0xAARRGGBB little-endian (what Ppu#buffer holds)
module Rubyboy
  class EmulatorWeb
    def initialize(rom_data)
      rom = Rom.new(rom_data)
      ram = Ram.new
      mbc = Cartridge::Factory.create(rom, ram)
      interrupt = Interrupt.new
      @ppu = Ppu.new(interrupt)
      @timer = Timer.new(interrupt)
      @joypad = Joypad.new(interrupt)
      @apu = Apu.new
      @bus = Bus.new(@ppu, rom, ram, mbc, @timer, interrupt, @joypad, @apu)
      @cpu = Cpu.new(@bus, interrupt)
    end

    def step(direction_key, action_key)
      @joypad.direction_button(direction_key)
      @joypad.action_button(action_key)
      loop do
        cycles = @cpu.exec
        @timer.step(cycles)
        return @ppu.buffer if @ppu.step(cycles)
      end
    end
  end
end

# rubyboy wants the ROM as an array of bytes, not a String (Rom#load_data
# compares the logo against an array of hex strings).
emu = Rubyboy::EmulatorWeb.new(File.binread(ROM_PATH).bytes)

# No palette: the Game Boy buffer is already 32-bit colour.  The marker keeps
# the wire format the same as the other pages, with an empty palette.
$stdout.write("PAL0")
$stdout.write(([0] * 768).pack("C*"))

loop do
  b = STDIN.read(1)
  break if b.nil?
  v = b.unpack1("C")
  break if v == 0xfe
  inv = (~v) & 0xff                      # pressed-high -> the joypad's active-low
  buf = emu.step(inv & 0x0f, (inv >> 4) & 0x0f)
  $stdout.write(buf.pack("L<*"))
  $stdout.flush
end
BODY
} > "$OUT"
echo "$OUT"
