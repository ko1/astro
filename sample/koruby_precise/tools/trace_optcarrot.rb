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

n = (ENV["TRACE_FRAMES"] || "10").to_i

n.times do |i|
  nes.send(:step)
  pixels = ppu.instance_variable_get(:@output_pixels)
  csum = pixels.pack("C*").sum
  uniq = {}
  pixels.each {|v| uniq[v] = (uniq[v] || 0) + 1 }
  keys_sorted = uniq.keys.sort
  uniq_str = keys_sorted.map {|k| "#{k}=#{uniq[k]}" }.join(",")
  pc      = cpu.instance_variable_get(:@_pc)
  a       = cpu.instance_variable_get(:@_a)
  x       = cpu.instance_variable_get(:@_x)
  y       = cpu.instance_variable_get(:@_y)
  sp      = cpu.instance_variable_get(:@_sp)
  pnz     = cpu.instance_variable_get(:@_p_nz)
  pc_     = cpu.instance_variable_get(:@_p_c)
  pv      = cpu.instance_variable_get(:@_p_v)
  pi      = cpu.instance_variable_get(:@_p_i)
  pd      = cpu.instance_variable_get(:@_p_d)
  clk     = cpu.instance_variable_get(:@clk)
  total   = cpu.instance_variable_get(:@clk_total)
  cframe  = cpu.instance_variable_get(:@clk_frame)
  hclk    = ppu.instance_variable_get(:@hclk)
  vclk    = ppu.instance_variable_get(:@vclk)
  htar    = ppu.instance_variable_get(:@hclk_target)
  scanl   = ppu.instance_variable_get(:@scanline)
  npix    = pixels.length
  printf("frame=%-3d csum=%-6d uniq={%s}\n", i, csum, uniq_str)
  $stdout.flush rescue nil
end
