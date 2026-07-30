# render_span_kernel.rb — extract of DOOM's hottest method (draw_span).
#
# In a perf profile of `koruby AOT-cached` rendering DOOM (E1M1), the SD for
# draw_span's inner while-loop was the single hottest baked SD (~11%), and the
# per-pixel float→int texcoord math (Float#to_i) + Array#[] table reads drove
# korb_send_cached / korb_m_flt_to_i.  This isolates that per-pixel work into a
# self-contained float kernel (mirrors lib/doom/render/renderer.rb#draw_span,
# the `while x <= x2` body) for stable measurement in isolation.
#
# Sustained ~1s scale: WIDTH columns × ROWS spans.

WIDTH = 320
ROWS  = 14_000

# Precomputed per-column tables (floats) — as draw_span caches from ivars.
column_distscale = Array.new(WIDTH) { |x| 0.5 + (x % 97) * 0.013 }
column_cos       = Array.new(WIDTH) { |x| ((x * 7) % 128) * 0.0078 - 0.5 }
column_sin       = Array.new(WIDTH) { |x| ((x * 11) % 128) * 0.0078 - 0.5 }
flat_pixels      = Array.new(64 * 64) { |i| (i * 37) & 0xff }
cmap             = Array.new(256) { |i| (i * 0x01_01_01) & 0xff_ffff }
framebuffer      = Array.new(WIDTH * ROWS, 0)

player_x     = 1024.5
neg_player_y = -768.25

acc = 0
row = 0
while row < ROWS
  # per-span setup (as draw_span does before its inner loop)
  perp_dist  = 32.0 + (row % 200) * 1.5
  row_offset = row * WIDTH
  x = 0
  while x < WIDTH
    ray_dist = perp_dist * column_distscale[x]
    tex_x = (player_x + ray_dist * column_cos[x]).to_i & 63
    tex_y = (neg_player_y - ray_dist * column_sin[x]).to_i & 63
    color = flat_pixels[tex_y * 64 + tex_x]
    framebuffer[row_offset + x] = cmap[color]
    x += 1
  end
  row += 1
end

# deterministic checksum of the framebuffer (CRuby vs koruby must agree)
i = 0
n = framebuffer.length
while i < n
  acc = (acc * 1_000_003 + framebuffer[i]) & 0xffff_ffff_ffff_ffff
  i += 4001   # stride-sample so the checksum is cheap but covers the buffer
end
p acc
