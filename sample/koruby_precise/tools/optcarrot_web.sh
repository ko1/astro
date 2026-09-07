#!/bin/sh
# optcarrot_web.sh — build the interactive NES bundle for the browser (wasm).
#
# Same engine files as optcarrot.sh, but the tail is a frame loop instead of a
# fixed frame count + checksum: one byte of pad state per frame on stdin, one
# raw 256x240 palette-indexed frame on stdout.  The host (a Worker) drives both
# ends, so nothing here needs a window, a timer, or a thread.
#
# Env:
#   ROM   path the bundle opens the ROM at (default /nes/rom.nes, where the
#         browser shim preopens it)
#   OUT   bundle path (default /tmp/optcarrot_web.rb)
set -e

HERE=$(cd "$(dirname "$0")/.." && pwd)
OPT="$HERE/../abruby/benchmark/optcarrot"
ROM=${ROM:-/nes/rom.nes}
OUT=${OUT:-/tmp/optcarrot_web.rb}

{
  # File shim: koruby_precise has only the __binread C primitive.
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
  def self.clock_gettime(clk) = __clock_gettime
end
SHIM
  for f in lib/optcarrot.rb lib/optcarrot/opt.rb lib/optcarrot/nes.rb \
           lib/optcarrot/rom.rb lib/optcarrot/pad.rb lib/optcarrot/cpu.rb \
           lib/optcarrot/apu.rb lib/optcarrot/ppu.rb lib/optcarrot/palette.rb \
           lib/optcarrot/driver.rb lib/optcarrot/config.rb \
           lib/optcarrot/mapper/mmc1.rb lib/optcarrot/mapper/uxrom.rb \
           lib/optcarrot/mapper/cnrom.rb lib/optcarrot/mapper/mmc3.rb; do
    grep -vE '^[[:space:]]*require_relative|^[[:space:]]*require ' "$OPT/$f"; echo
  done
  cat <<BODY
ROM_PATH = "$ROM"
BODY
  cat <<'BODY'
# --- interactive driver ------------------------------------------------------
# Protocol, one byte in / one frame out, so the host stays in control of pacing:
#   in   bit 0..7 = A B Select Start Up Down Left Right (NES pad order), 0xff = quit
#   out  256 * 240 bytes, palette indices
module Optcarrot
  class NES
    attr_reader :pads          # the benchmark driver never needed these
  end
end

nes  = Optcarrot::NES.new(["-b", "--frames", "0", ROM_PATH])
pads = nes.pads
ppu  = nes.ppu
nes.reset

# The palette the page needs to colour the indices: 64 entries x 3 bytes,
# after a 4-byte marker.  Index 0..63 is what output_pixels holds.
pal = Optcarrot::Palette.defacto_palette
bytes = []
64.times do |i|
  c = pal[i]
  if c.is_a?(Array)
    bytes << (c[0] & 0xff) << (c[1] & 0xff) << (c[2] & 0xff)
  else
    bytes << ((c >> 16) & 0xff) << ((c >> 8) & 0xff) << (c & 0xff)
  end
end
$stdout.write("PAL0")
$stdout.write(bytes.pack("C*"))

held = 0
loop do
  b = STDIN.read(1)
  break if b.nil?
  v = b.unpack1("C")
  break if v == 0xff
  # Only the bits that changed need a keydown/keyup.
  8.times do |i|
    now = (v >> i) & 1
    was = (held >> i) & 1
    next if now == was
    now == 1 ? pads.keydown(0, i) : pads.keyup(0, i)
  end
  held = v

  nes.step
  $stdout.write(ppu.output_pixels.pack("C*"))
  $stdout.flush
end
BODY
} > "$OUT"
echo "$OUT"
