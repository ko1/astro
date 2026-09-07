#!/bin/sh
# rnes_web.sh — build the interactive NES bundle for the browser (wasm).
#
# Uses r7kamura/rnes rather than optcarrot: optcarrot's PPU drives the CPU
# through a Fiber, and koruby's wasm port has no Fiber.  rnes steps the PPU
# inline, so it runs anywhere.
#
# Hooks, all small:
#   - PartsFactory#renderer returns a sink whose #render sets a "frame done"
#     flag.  The PPU already calls it once per frame; nothing else is needed.
#   - Emulator exposes @ppu / @keypad1 so the driver can reach them.
#   - Keypad#check is neutered: it reads STDIN, which is our control channel.
#   - Image exposes its byte array.
#
# Env:
#   ROM   path the bundle opens the ROM at (default /nes/rom.nes)
#   OUT   bundle path (default /tmp/rnes_web.rb)
set -e

HERE=$(cd "$(dirname "$0")/.." && pwd)
APP=${RNES:-/tmp/rnes}
ROM=${ROM:-/nes/rom.nes}
OUT=${OUT:-/tmp/rnes_web.rb}

[ -d "$APP/lib/rnes" ] || { echo "rnes not found at $APP (git clone https://github.com/r7kamura/rnes)" >&2; exit 1; }

{
  cat <<'SHIM'
unless File.respond_to?(:binread)
  class File
    def self.binread(path) = __binread(path)
    def self.read(path) = __binread(path)
  end
end
SHIM
  for f in lib/rnes/version.rb lib/rnes/errors.rb lib/rnes/image.rb \
           lib/rnes/ram.rb lib/rnes/rom.rb lib/rnes/ines_header.rb \
           lib/rnes/rom_loader.rb lib/rnes/interrupt_line.rb \
           lib/rnes/keypad.rb lib/rnes/ppu/colors.rb lib/rnes/ppu_registers.rb \
           lib/rnes/ppu_bus.rb lib/rnes/terminal_renderer.rb lib/rnes/ppu.rb \
           lib/rnes/cpu_registers.rb lib/rnes/operation/records.rb \
           lib/rnes/operation.rb lib/rnes/cpu_bus.rb lib/rnes/dma_controller.rb \
           lib/rnes/cpu.rb lib/rnes/logger.rb lib/rnes/parts_factory.rb \
           lib/rnes/emulator.rb; do
    grep -vE "^[[:space:]]*require(_relative)? " "$APP/$f"; echo
  done
  cat <<BODY
ROM_PATH = "$ROM"
BODY
  cat <<'BODY'
# --- interactive driver ------------------------------------------------------
# Protocol, one byte in / one frame out:
#   in   bit 0..7 = Right Left Down Up Start Select B A (rnes' own key order),
#        0xff = quit
#   out  256 * 240 * 3 bytes, RGB
module Rnes
  # The PPU calls renderer.render(image) exactly once per frame; that is the
  # only frame signal we need.
  class FrameSink
    attr_accessor :done
    def render(_image) = @done = true
  end

  class PartsFactory
    def renderer
      @renderer ||= ::Rnes::FrameSink.new
    end
  end

  class Emulator
    attr_reader :ppu, :keypad1, :renderer_sink
    def sink = @cpu_bus && nil || nil
  end

  class Keypad
    attr_accessor :buffer
    def check; end            # STDIN is the control channel here
  end

  class Image
    attr_reader :bytes
  end

  # rnes is Ruby-2-era: Operation.build does new(record) and relies on a Hash
  # auto-splatting into keywords, which Ruby 3 dropped.
  class Operation
    class << self
      def build(operation_code)
        record = ::Rnes::Operation::RECORDS[operation_code]
        raise ::Rnes::InvalidOperationCodeError, "Invalid operation code: #{operation_code}" unless record
        new(**record)
      end
    end
  end
end

emu     = Rnes::Emulator.new
factory = emu.instance_variable_get(:@ppu)
sink    = factory.instance_variable_get(:@renderer)
keypad  = emu.keypad1
ppu     = emu.ppu
emu.load_rom(File.binread(ROM_PATH).bytes)

# NES has no palette to hand over (rnes writes RGB directly); keep the wire
# format the same as the other pages with an empty palette block.
$stdout.write("PAL0")
$stdout.write(([0] * 768).pack("C*"))

loop do
  b = STDIN.read(1)
  break if b.nil?
  v = b.unpack1("C")
  break if v == 0xff
  keypad.buffer = v

  sink.done = false
  until sink.done
    emu.step
  end
  $stdout.write(ppu.image.bytes.flatten.pack("C*"))
  $stdout.flush
end
BODY
} > "$OUT"
echo "$OUT"
