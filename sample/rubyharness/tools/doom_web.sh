#!/bin/sh
# doom_web.sh — build the interactive DOOM bundle for the browser (wasm).
#
# Same engine files as doom.sh, but the tail is a frame loop instead of a fixed
# render + checksum: one byte of input per frame on stdin, one raw 320x200
# palette-indexed frame on stdout.  The host (a Worker) drives both ends, so
# nothing here needs a window, a timer, or a thread.
#
# Env:
#   WAD_PATH  path the bundle opens the WAD at (default /doom/doom1.wad,
#             which is where the browser shim preopens it)
#   OUT       bundle path (default /tmp/doom_web.rb)
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../apps/doom"
WAD=${WAD_PATH:-/doom/doom1.wad}
OUT=${OUT:-/tmp/doom_web.rb}

[ -f "$APP/doom1.wad" ] || sh "$HERE/doom_setup.sh"

{
  printf 'WAD_PATH = "%s"\n' "$WAD"
  echo 'module Doom; module Platform; class GosuWindow; end; end; end'
  for f in lib/doom/version.rb \
           lib/doom/wad/reader.rb lib/doom/wad/palette.rb lib/doom/wad/colormap.rb \
           lib/doom/wad/flat.rb lib/doom/wad/patch.rb lib/doom/wad/texture.rb \
           lib/doom/wad/sprite.rb lib/doom/wad/hud_graphics.rb \
           lib/doom/map/data.rb lib/doom/game/player_state.rb \
           lib/doom/game/sector_actions.rb lib/doom/game/animations.rb \
           lib/doom/game/sector_effects.rb lib/doom/render/renderer.rb \
           lib/doom/render/status_bar.rb lib/doom/render/weapon_renderer.rb; do
    grep -vE "^[[:space:]]*require(_relative)? " "$APP/$f"; echo
  done
  cat <<'BODY'
# --- interactive driver ------------------------------------------------------
# Protocol, one byte in / one frame out, so the host stays in control of pacing:
#   in   'w' 's' forward/back, 'a' 'd' strafe, 'j' 'l' turn, 'q' quit,
#        '.'     no input this frame
#   out  SCREEN_WIDTH * SCREEN_HEIGHT bytes, palette indices
wad      = Doom::Wad::Reader.new(WAD_PATH)
palette  = Doom::Wad::Palette.load(wad)
colormap = Doom::Wad::Colormap.load(wad)
flats    = Doom::Wad::Flat.load_all(wad)
textures = Doom::Wad::TextureManager.new(wad)
sprites  = Doom::Wad::SpriteManager.new(wad)
map      = Doom::Map::MapData.load(wad, 'E1M1')
renderer = Doom::Render::Renderer.new(wad, map, textures, palette, colormap, flats, sprites)
renderer.skip_background_fill = true

ps = map.player_start
px = ps.x.to_f
py = ps.y.to_f
pz = 41.0
pa = ps.angle.to_f          # degrees, as the renderer wants

# The palette is the one the page needs to colour the indices; hand it over
# once, before any frame, as 768 bytes after a 4-byte marker.
pal = palette.respond_to?(:colors) ? palette.colors : nil
if pal
  bytes = []
  pal.each do |c|
    if c.is_a?(Array)
      bytes << (c[0] & 0xff) << (c[1] & 0xff) << (c[2] & 0xff)
    else
      bytes << ((c >> 16) & 0xff) << ((c >> 8) & 0xff) << (c & 0xff)
    end
  end
  bytes = bytes[0, 768]
  bytes << 0 while bytes.length < 768
  $stdout.write("PAL0")
  $stdout.write(bytes.pack("C*"))
end

STEP = 12.0
TURN = 5.0
DEG  = 3.14159265358979 / 180.0

loop do
  cmd = STDIN.read(1)
  break if cmd.nil? || cmd == "q"
  case cmd
  when "w", "s"
    d = (cmd == "w") ? STEP : -STEP
    px += Math.cos(pa * DEG) * d
    py += Math.sin(pa * DEG) * d
  when "a", "d"
    d = (cmd == "a") ? STEP : -STEP
    px += Math.cos((pa + 90.0) * DEG) * d
    py += Math.sin((pa + 90.0) * DEG) * d
  when "j" then pa += TURN
  when "l" then pa -= TURN
  end
  pa -= 360.0 while pa >= 360.0
  pa += 360.0 while pa < 0.0

  renderer.set_player(px.to_i, py.to_i, pz.to_i, pa.to_i)
  renderer.render_frame
  $stdout.write(renderer.framebuffer.pack("C*"))
  $stdout.flush
end
BODY
} > "$OUT"
echo "$OUT"
