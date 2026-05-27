# bm_render_pixel_kernel.rb — extract of optcarrot's hottest method.
#
# In a perf profile of `koruby AOT-cached` running optcarrot,
# `SD_640496b25b4af69c` accounted for **22%** of total CPU.  That SD is
# the call-site dispatching to PPU#render_pixel from the per-scanline
# render loop.  This file isolates the same per-pixel work into a
# self-contained kernel so the inner-loop dispatcher and ivar/array
# access patterns can be benchmarked and profiled in isolation.
#
# Source mirrors lib/optcarrot/ppu.rb#render_pixel (lines 806-822).
# Driver loop simulates one scanline (256 calls) × N scanlines so the
# benchmark runs at sustained ~1-second scale for stable measurement.

class PPUKernel
  HCLOCK_DUMMY = 341 * 241 # arbitrary positive sentinel from optcarrot
  PALETTE_SIZE = 64

  def initialize
    # Match the runtime shape of optcarrot's PPU at "rendering" time.
    @any_show     = true
    @bg_enabled   = true
    @sp_active    = true
    @bg_pixels    = Array.new(8) { |i| i * 4 }
    @sp_map       = Array.new(341)
    # Pre-populate sprite hits at half the columns so we exercise both
    # branches of `if @sp_active && (sprite = @sp_map[@hclk])`.
    341.times do |i|
      next if i.odd?
      # sprite = [behind, sprite_zero, palette_idx]
      @sp_map[i] = [i % 3 == 0, i % 5 == 0, (i % PALETTE_SIZE) | 0x10]
    end
    @scroll_addr_5_14 = 0x2000
    @scroll_addr_0_4  = 0
    @output_color = Array.new(PALETTE_SIZE * 8) { |i| i * 0x010101 }
    @output_pixels = []
    @hclk = 0
    @sp_zero_hit = false
  end

  # Verbatim copy of the hot kernel from ppu.rb.
  def render_pixel
    if @any_show
      pixel = @bg_enabled ? @bg_pixels[@hclk % 8] : 0
      if @sp_active && (sprite = @sp_map[@hclk])
        if pixel % 4 == 0
          pixel = sprite[2]
        else
          @sp_zero_hit = true if sprite[1] && @hclk != 255
          pixel = sprite[2] unless sprite[0]
        end
      end
    else
      pixel = @scroll_addr_5_14 & 0x3f00 == 0x3f00 ? @scroll_addr_0_4 : 0
      @bg_pixels[@hclk % 8] = 0
    end
    @output_pixels << @output_color[pixel]
  end

  # Driver: simulate `iters` full scanlines (256 pixels each) so the
  # hot loop dominates startup / GC noise.
  def run(iters)
    iters.times do
      @output_pixels.clear if @output_pixels.size > 1_000_000  # bound memory
      256.times do |i|
        @hclk = i
        render_pixel
      end
    end
  end
end

iters = (ARGV[0] || 4_000).to_i
ppu = PPUKernel.new
t0 = Time.now
ppu.run(iters)
t1 = Time.now

puts "iters=#{iters} scanlines, pixels=#{iters * 256}"
puts "elapsed=#{(t1 - t0).round(3)}s, rate=#{(iters * 256 / (t1 - t0)).round(0)} px/s"
