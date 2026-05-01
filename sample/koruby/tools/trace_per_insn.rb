# Per-instruction CPU+PPU trace.  Drives optcarrot N frames, dumping
# (PC, A, X, Y, P, SP, clk, opcode, hclk, vclk) before every CPU
# instruction.  Stream goes to stdout; redirect each runtime separately
# and diff to find the first divergent instruction.
#
# Args (env):
#   TRACE_FRAMES — frames to advance before tracing (default 0; trace from boot)
#   TRACE_INSNS  — max instructions to log (default 200000)
#   START_FRAME  — frame number when tracing turns on (default 5)

OPT_DIR = File.expand_path("../../abruby/benchmark/optcarrot", __dir__)
require_relative "../../abruby/benchmark/optcarrot/lib/optcarrot"

argv = [
  "--headless",
  "--no-print-fps",
  "--no-print-video-checksum",
  File.join(OPT_DIR, "examples", "Lan_Master.nes")
]
nes = Optcarrot::NES.new(argv)
nes.send(:reset)
cpu = nes.cpu
ppu = nes.ppu

$trace_on = false      # gates per-CPU-instruction lines
$phase_on = false      # gates frame phase boundary dumps
$logged = 0
$max_log = (ENV["TRACE_INSNS"] || "200000").to_i
$start_frame = (ENV["START_FRAME"] || "5").to_i
$nframes = (ENV["TRACE_FRAMES"] || "8").to_i

# Replace CPU#run with a tracing version.  Mirrors the original method
# (cpu.rb:926) but inlines dispatch and dumps state per opcode.
class Optcarrot::CPU
  def trace_run
    do_clock
    begin
      begin
        @opcode = fetch(@_pc)
        if $trace_on && $logged < $max_log
          flags = @_p_nz | @_p_c | @_p_v | @_p_i | @_p_d
          bg_pix = @ppu.instance_variable_get(:@bg_pixels)
          npix = @ppu.instance_variable_get(:@output_pixels).length
          $stdout.write(sprintf("CPU pc=%04x a=%02x x=%02x y=%02x sp=%02x p_nz=%-4x p_c=%d p_v=%d p_i=%d p_d=%d clk=%-12d op=%02x hclk=%-7d vclk=%-7d npix=%-6d bg=%s\n",
            @_pc, @_a, @_x, @_y, @_sp, @_p_nz, @_p_c, @_p_v, @_p_i, @_p_d, @clk, @opcode, @ppu.instance_variable_get(:@hclk), @ppu.instance_variable_get(:@vclk), npix, bg_pix.inspect))
          $logged += 1
          if $logged == $max_log
            $stdout.write("--- TRACE TRUNCATED at #{$max_log} insns ---\n")
            $trace_on = false
          end
        end
        @_pc += 1
        send(*DISPATCH[@opcode])
        @ppu.sync(@clk) if @ppu_sync
      end while @clk < @clk_target
      do_clock
    end while @clk < @clk_frame
    if $phase_on
      bg_pix = @ppu.instance_variable_get(:@bg_pixels)
      bg_pat = @ppu.instance_variable_get(:@bg_pattern)
      hclk = @ppu.instance_variable_get(:@hclk)
      vclk = @ppu.instance_variable_get(:@vclk)
      $stdout.write(sprintf("END-CPU.run clk=%d hclk=%d vclk=%d bg_pat=%d bg=%s\n", @clk, hclk, vclk, bg_pat, bg_pix.inspect))
    end
  end
  def run
    trace_run
  end
end

$stdout.write("before-step  output_pixels.nil?=#{ppu.instance_variable_get(:@output_pixels).nil?}\n")
def dump_attr_lut(ppu, label)
  attr_lut = ppu.instance_variable_get(:@attr_lut)
  return unless attr_lut
  return unless attr_lut.is_a?(Hash) || attr_lut.is_a?(Array)
  # show a few specific entries
  scroll04 = ppu.instance_variable_get(:@scroll_addr_0_4)
  scroll514 = ppu.instance_variable_get(:@scroll_addr_5_14)
  key = (scroll04 || 0) + (scroll514 || 0)
  ent = attr_lut[key]
  if ent
    lut_arr = ent[1]
    idx = nil
    4.times {|k| idx = k if Optcarrot::PPU::TILE_LUT[k].equal?(lut_arr) }
    $stdout.write(sprintf("%s attr_lut[%d]=[io=%s, lut_idx=%s, shift=%s]\n", label, key, ent[0].inspect, idx.inspect, ent[2].inspect))
  else
    $stdout.write(sprintf("%s attr_lut[%d]=nil\n", label, key))
  end
end

def dump_ppu(ppu, label)
  bg_pix = ppu.instance_variable_get(:@bg_pixels)
  bg_pat = ppu.instance_variable_get(:@bg_pattern)
  io_addr = ppu.instance_variable_get(:@io_addr)
  hclk = ppu.instance_variable_get(:@hclk)
  vclk = ppu.instance_variable_get(:@vclk)
  htar = ppu.instance_variable_get(:@hclk_target)
  scroll_xfine = ppu.instance_variable_get(:@scroll_xfine)
  bg_lut = ppu.instance_variable_get(:@bg_pattern_lut)
  bg_lut_fetched = ppu.instance_variable_get(:@bg_pattern_lut_fetched)
  lut_idx = nil; fetched_idx = nil
  4.times do |k|
    lut_idx = k if Optcarrot::PPU::TILE_LUT[k].equal?(bg_lut)
    fetched_idx = k if Optcarrot::PPU::TILE_LUT[k].equal?(bg_lut_fetched)
  end
  lut_at_pat = bg_lut ? bg_lut[bg_pat || 0] : nil
  $stdout.write(sprintf("%s hclk=%d vclk=%d htar=%d io_addr=%d xfine=%d lut_idx=%s fetched_idx=%s lut[%d]=%s bg_pat=%d bg=%s\n",
                         label, hclk, vclk, htar, io_addr, scroll_xfine || 0,
                         lut_idx.inspect, fetched_idx.inspect,
                         bg_pat || 0, lut_at_pat.inspect, bg_pat, bg_pix.inspect))
end

# Hook PPU functions that touch bg_pattern_lut.
class Optcarrot::PPU
  def trace_make_sure_invariants
    @name_io_addr = (@scroll_addr_0_4 | @scroll_addr_5_14) & 0x0fff | 0x2000
    nbank = @nmt_ref[@io_addr >> 10 & 3]
    nidx = @io_addr & 0x03ff
    nbyte = nbank[nidx]
    shift = (@scroll_addr_0_4 & 0x2) | (@scroll_addr_5_14[6] * 0x4)
    final_idx = nbyte >> shift & 3
    @bg_pattern_lut_fetched = Optcarrot::PPU::TILE_LUT[final_idx]
    if $phase_on
      $stdout.write(sprintf("MSI io=%d nb_idx=%d nbank_idx=%d nbyte=%d shift=%d final=%d\n",
        @io_addr, @io_addr >> 10 & 3, nidx, nbyte, shift, final_idx))
    end
  end
  def make_sure_invariants
    trace_make_sure_invariants
  end

  $fetch_attr_count = 0
  def fetch_attr
    return unless @any_show
    @bg_pattern_lut = @bg_pattern_lut_fetched
    if $phase_on
      $fetch_attr_count += 1
      lut = @bg_pattern_lut
      idx = nil
      4.times {|k| idx = k if Optcarrot::PPU::TILE_LUT[k].equal?(lut) }
      $stdout.write(sprintf("FA[%d] hclk=%d vclk=%d io=%d lut_idx=%s\n",
        $fetch_attr_count, @hclk, @vclk, @io_addr, idx.inspect))
    end
  end

  $fetch_bg_count = 0
  def fetch_bg_pattern_0
    return unless @any_show
    @bg_pattern = @chr_mem[@io_addr & 0x1fff]
    if $phase_on
      $fetch_bg_count += 1
      $stdout.write(sprintf("FBG0[%d] io=%d bg_pat=%d chr[%d]=%d\n",
        $fetch_bg_count, @io_addr, @bg_pattern, @io_addr & 0x1fff, @chr_mem[@io_addr & 0x1fff]))
    end
  end

  def fetch_bg_pattern_1
    return unless @any_show
    @bg_pattern |= @chr_mem[@io_addr & 0x1fff] * 0x100
    if $phase_on
      $stdout.write(sprintf("FBG1[%d] io=%d bg_pat=%d chr[%d]=%d\n",
        $fetch_bg_count, @io_addr, @bg_pattern, @io_addr & 0x1fff, @chr_mem[@io_addr & 0x1fff]))
    end
  end
end
class Optcarrot::NES
  def trace_step(idx)
    @ppu.setup_frame
    if $phase_on
      $stdout.write(sprintf("AFTER-setup_frame.%d ", idx))
      dump_ppu(@ppu, "")
    end
    @cpu.run
    if $phase_on
      $stdout.write(sprintf("AFTER-cpu.run.%d ", idx))
      dump_ppu(@ppu, "")
    end
    @ppu.vsync
    if $phase_on
      $stdout.write(sprintf("AFTER-ppu.vsync.%d ", idx))
      dump_ppu(@ppu, "")
    end
    @apu.vsync
    @cpu.vsync
    @rom.vsync
    @input.tick(@frame, @pads)
    @fps = @video.tick(@ppu.output_pixels)
    @audio.tick(@apu.output)
    @frame += 1
  end
end

$nframes.times do |i|
  $phase_on = (i >= $start_frame)
  $trace_on = $phase_on && $logged < $max_log
  if $phase_on
    dump_ppu(ppu, "PRE-step.#{i}")
    dump_attr_lut(ppu, "PRE-step.#{i}")
  end
  begin
    nes.send(:trace_step, i)
  rescue => e
    $stdout.write("step #{i} EXC #{e.class}: #{e.message}\n")
    raise
  end
  if $phase_on
    dump_ppu(ppu, "POST-step.#{i}")
  end
  pixels = ppu.instance_variable_get(:@output_pixels)
  csum = pixels ? pixels.pack("C*").sum : -1
  $stdout.write(sprintf("FRAME %d csum=%d insns_logged=%d pixels.nil?=%s\n", i, csum, $logged, pixels.nil?))
  $stdout.flush rescue nil
end
