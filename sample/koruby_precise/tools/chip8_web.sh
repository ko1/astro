#!/bin/sh
# chip8_web.sh — build the interactive CHIP-8 bundle for the browser (wasm).
#
# The interpreter is written here rather than borrowed: CHIP-8 is small enough
# that it is less work than adapting someone else's, and it keeps the bundle's
# provenance simple.  Same one byte in / one frame out protocol as the other
# pages, so the Worker side is shared.
#
# SUPER-CHIP is not implemented; the original 64x32 instruction set is.
#
# Env:
#   ROM   path the bundle opens the ROM at (default /c8/rom.ch8)
#   OUT   bundle path (default /tmp/chip8_web.rb)
set -e
ROM=${ROM:-/c8/rom.ch8}
OUT=${OUT:-/tmp/chip8_web.rb}

{
  cat <<'SHIM'
unless File.respond_to?(:binread)
  class File
    def self.binread(path) = __binread(path)
    def self.read(path) = __binread(path)
  end
end
SHIM
  cat <<BODY
ROM_PATH = "$ROM"
BODY
  cat <<'BODY'
# --- CHIP-8 ------------------------------------------------------------------
# 4 KB of memory, 16 8-bit registers, a 64x32 monochrome screen drawn by XORing
# sprites (a collision sets VF), two 60 Hz timers, and a 16-key hex keypad.
class Chip8
  WIDTH = 64
  HEIGHT = 32
  FONT = [
    0xF0, 0x90, 0x90, 0x90, 0xF0,  0x20, 0x60, 0x20, 0x20, 0x70,
    0xF0, 0x10, 0xF0, 0x80, 0xF0,  0xF0, 0x10, 0xF0, 0x10, 0xF0,
    0x90, 0x90, 0xF0, 0x10, 0x10,  0xF0, 0x80, 0xF0, 0x10, 0xF0,
    0xF0, 0x80, 0xF0, 0x90, 0xF0,  0xF0, 0x10, 0x20, 0x40, 0x40,
    0xF0, 0x90, 0xF0, 0x90, 0xF0,  0xF0, 0x90, 0xF0, 0x10, 0xF0,
    0xF0, 0x90, 0xF0, 0x90, 0x90,  0xE0, 0x90, 0xE0, 0x90, 0xE0,
    0xF0, 0x80, 0x80, 0x80, 0xF0,  0xE0, 0x90, 0x90, 0x90, 0xE0,
    0xF0, 0x80, 0xF0, 0x80, 0xF0,  0xF0, 0x80, 0xF0, 0x80, 0x80
  ].freeze

  attr_reader :screen

  def initialize(rom_bytes)
    @mem = Array.new(4096, 0)
    FONT.each_with_index { |b, i| @mem[i] = b }
    rom_bytes.each_with_index { |b, i| @mem[0x200 + i] = b }
    @v = Array.new(16, 0)
    @i = 0
    @pc = 0x200
    @stack = []
    @dt = 0
    @st = 0
    @keys = 0
    @screen = Array.new(WIDTH * HEIGHT, 0)
    @wait_reg = nil
    @rand = 0x2545F491
  end

  def keys=(bits)
    @keys = bits
  end

  # xorshift: the host has no business seeding this, and Kernel#rand would drag
  # in a generator the bundle does not otherwise need.
  def next_random
    @rand ^= (@rand << 13) & 0xffffffff
    @rand ^= @rand >> 17
    @rand ^= (@rand << 5) & 0xffffffff
    @rand & 0xff
  end

  # One frame: `cycles` instructions, then the timers tick once at 60 Hz.
  def frame(cycles)
    cycles.times { step }
    @dt -= 1 if @dt > 0
    @st -= 1 if @st > 0
  end

  def step
    if @wait_reg
      16.times do |k|
        next if (@keys >> k) & 1 == 0
        @v[@wait_reg] = k
        @wait_reg = nil
        break
      end
      return if @wait_reg
    end

    op = (@mem[@pc] << 8) | @mem[@pc + 1]
    @pc = (@pc + 2) & 0xfff
    x   = (op >> 8) & 0xf
    y   = (op >> 4) & 0xf
    n   = op & 0xf
    nn  = op & 0xff
    nnn = op & 0xfff

    case op >> 12
    when 0x0
      if op == 0x00e0
        @screen = Array.new(WIDTH * HEIGHT, 0)
      elsif op == 0x00ee
        @pc = @stack.pop || 0x200
      end
    when 0x1 then @pc = nnn
    when 0x2 then @stack.push(@pc); @pc = nnn
    when 0x3 then @pc = (@pc + 2) & 0xfff if @v[x] == nn
    when 0x4 then @pc = (@pc + 2) & 0xfff if @v[x] != nn
    when 0x5 then @pc = (@pc + 2) & 0xfff if @v[x] == @v[y]
    when 0x6 then @v[x] = nn
    when 0x7 then @v[x] = (@v[x] + nn) & 0xff
    when 0x8
      case n
      when 0x0 then @v[x] = @v[y]
      when 0x1 then @v[x] |= @v[y]
      when 0x2 then @v[x] &= @v[y]
      when 0x3 then @v[x] ^= @v[y]
      when 0x4
        s = @v[x] + @v[y]
        @v[x] = s & 0xff
        @v[0xf] = s > 0xff ? 1 : 0
      when 0x5
        d = @v[x] - @v[y]
        @v[x] = d & 0xff
        @v[0xf] = d < 0 ? 0 : 1
      when 0x6
        f = @v[x] & 1
        @v[x] = @v[x] >> 1
        @v[0xf] = f
      when 0x7
        d = @v[y] - @v[x]
        @v[x] = d & 0xff
        @v[0xf] = d < 0 ? 0 : 1
      when 0xe
        f = (@v[x] >> 7) & 1
        @v[x] = (@v[x] << 1) & 0xff
        @v[0xf] = f
      end
    when 0x9 then @pc = (@pc + 2) & 0xfff if @v[x] != @v[y]
    when 0xa then @i = nnn
    when 0xb then @pc = (nnn + @v[0]) & 0xfff
    when 0xc then @v[x] = next_random & nn
    when 0xd
      vx = @v[x]
      vy = @v[y]
      hit = 0
      n.times do |row|
        byte = @mem[(@i + row) & 0xfff]
        8.times do |col|
          next if (byte >> (7 - col)) & 1 == 0
          px = (vx + col) % WIDTH
          py = (vy + row) % HEIGHT
          idx = py * WIDTH + px
          hit = 1 if @screen[idx] == 1
          @screen[idx] ^= 1
        end
      end
      @v[0xf] = hit
    when 0xe
      pressed = (@keys >> (@v[x] & 0xf)) & 1
      if nn == 0x9e
        @pc = (@pc + 2) & 0xfff if pressed == 1
      elsif nn == 0xa1
        @pc = (@pc + 2) & 0xfff if pressed == 0
      end
    when 0xf
      case nn
      when 0x07 then @v[x] = @dt
      when 0x0a then @wait_reg = x
      when 0x15 then @dt = @v[x]
      when 0x18 then @st = @v[x]
      when 0x1e then @i = (@i + @v[x]) & 0xfff
      when 0x29 then @i = (@v[x] & 0xf) * 5
      when 0x33
        v = @v[x]
        @mem[@i] = v / 100
        @mem[@i + 1] = (v / 10) % 10
        @mem[@i + 2] = v % 10
      when 0x55 then (0..x).each { |k| @mem[(@i + k) & 0xfff] = @v[k] }
      when 0x65 then (0..x).each { |k| @v[k] = @mem[(@i + k) & 0xfff] }
      end
    end
  end
end

# --- interactive driver ------------------------------------------------------
# Protocol: two bytes in (the 16-key state, low then high), one frame out as
# 64*32 bytes of 0 or 1.  0xff 0xff = quit.
CYCLES = 15                       # instructions per frame; Octo's default rate

emu = Chip8.new(File.binread(ROM_PATH).bytes)

$stdout.write("PAL0")
$stdout.write(([0] * 768).pack("C*"))

loop do
  a = STDIN.read(1)
  break if a.nil?
  b = STDIN.read(1)
  break if b.nil?
  lo = a.unpack1("C")
  hi = b.unpack1("C")
  break if lo == 0xff && hi == 0xff
  emu.keys = lo | (hi << 8)
  emu.frame(CYCLES)
  $stdout.write(emu.screen.pack("C*"))
  $stdout.flush
end
BODY
} > "$OUT"
echo "$OUT"
