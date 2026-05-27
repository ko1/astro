# Kernel benchmark: PPU#render_pixel pattern.
# Mimics optcarrot's hot loop: many ivar gets, hash[key % small], array<<,
# guarded ivar access via boolean flags.

class PPU
  def initialize
    @any_show     = true
    @bg_enabled   = true
    @sp_active    = true
    @bg_pixels    = [0, 1, 2, 3, 4, 5, 6, 7]
    @sp_map       = {}
    256.times do |i|
      if i % 5 == 0
        @sp_map[i] = nil
      elsif i % 7 != 0
        @sp_map[i] = [false, true, i & 0x3f]
      end
    end
    @sp_zero_hit  = false
    @hclk         = 0
    @output_pixels = []
    @output_color = (0...64).to_a
    @scroll_addr_5_14 = 0
    @scroll_addr_0_4  = 0
  end

  def render_pixel
    if @any_show
      pixel = @bg_enabled ? @bg_pixels[@hclk % 8] : 0
      sprite = @sp_active ? @sp_map[@hclk] : nil
      if sprite
        if pixel % 4 == 0
          pixel = sprite[2]
        else
          if sprite[1] && @hclk < 255
            @sp_zero_hit = true
          end
          pixel = sprite[2] unless sprite[0]
        end
      end
    else
      pixel = @scroll_addr_5_14 & 0x3f00 == 0x3f00 ? @scroll_addr_0_4 : 0
      @bg_pixels[@hclk % 8] = 0
    end
    @output_pixels << @output_color[pixel]
  end

  def run(iters)
    iters.times do
      h = 0
      while h < 256
        @hclk = h
        render_pixel
        h = h + 1
      end
      @output_pixels.clear
    end
  end
end

ppu = PPU.new
t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
ppu.run(20000)
t1 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
puts "render_pixel x 5.12M: #{((t1 - t0) * 1000).to_i} ms"
