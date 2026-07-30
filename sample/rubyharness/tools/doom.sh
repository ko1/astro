#!/bin/sh
# doom.sh — pure-Ruby DOOM headless render benchmark for the rubyharness model.
#
# Bundles the DOOM engine into one require-free file (koruby has no `require`),
# renders a fixed number of frames from a fixed viewpoint, and prints a
# framebuffer checksum (deterministic — CRuby and an ASTro sample must agree).
# The harness/user times the wall clock externally; correctness = matching
# checksum (like optcarrot).
#
# Env:
#   DOOM_MODE   plain (default) — run the tree-walker (INTERP --plain)
#               aot             — bake (--aot-compile) then run --compiled-only
#               cruby           — run with $RUBY (oracle)
#               build           — only write the bundle, print its path, exit
#   INTERP      the sample binary            (default ./koruby_precise)
#   RUBY        reference ruby               (default ruby)
#   FRAMES      frames to render             (default 60)
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/../apps/doom"
MODE=${DOOM_MODE:-plain}
INTERP=${INTERP:-./koruby_precise}
RUBY=${RUBY:-ruby}
FRAMES=${FRAMES:-60}
BUNDLE=${DOOM_BUNDLE:-/tmp/doom_bundle.rb}

[ -f "$APP/doom1.wad" ] || sh "$HERE/doom_setup.sh"

# Absolutize INTERP (we cd into $APP for the AOT code_store below).
case "$INTERP" in /*|*" "*) ;; ./*) INTERP="$(cd "$(dirname "$INTERP")" && pwd)/$(basename "$INTERP")";; esac

# --- build the bundle (requires stripped, in dependency order) ---
{
  printf 'FRAMES = %s\nWAD_PATH = "%s/doom1.wad"\n' "$FRAMES" "$APP"
  echo 'module Doom; module Platform; class GosuWindow; end; end; end'   # headless: no window
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
renderer.set_player(ps.x, ps.y, 41, ps.angle)
FRAMES.times { renderer.render_frame }
fb = renderer.framebuffer
sum = 1; i = 0; n = fb.length
while i < n; sum = (sum * 1_000_003 + fb[i]) & 0xffff_ffff_ffff_ffff; i += 1; end
puts "frames: #{FRAMES}"
puts "pixels: #{n}"
puts "checksum: #{sum}"
BODY
} > "$BUNDLE"

case "$MODE" in
  build) echo "$BUNDLE"; exit 0 ;;
  cruby) exec "$RUBY" "$BUNDLE" ;;
  aot)
    cd "$APP"; rm -rf code_store
    CCACHE_DISABLE=1 "$INTERP" --aot-compile "$BUNDLE" >/dev/null 2>&1 || { echo "aot-compile failed"; exit 1; }
    exec "$INTERP" --compiled-only "$BUNDLE" ;;
  plain|*) exec "$INTERP" --plain "$BUNDLE" ;;
esac
