#!/bin/sh
# rbtris_web.sh — build the interactive rbTris bundle for the browser (wasm).
#
# rbtris is a Ruby2D game.  Ruby2D binds to SDL through a C extension, which
# neither koruby nor wasm can load — but nothing about its *API* is native: it
# is a retained list of shapes a renderer walks.  tools/ruby2d_shim.rb keeps the
# shapes and rasterises them, so the game itself runs unmodified.
#
# The game's own file is used verbatim except for two lines that cannot work
# here: the font download at the top (we have a built-in bitmap font) and
# `require "ruby2d"`.
#
# Env:
#   RBTRIS  checkout of https://github.com/Nakilon/rbtris (default /tmp/rbtris)
#   OUT     bundle path (default /tmp/rbtris_web.rb)
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
APP=${RBTRIS:-/tmp/rbtris}
OUT=${OUT:-/tmp/rbtris_web.rb}

[ -f "$APP/main.rb" ] || { echo "rbtris not found at $APP" >&2; exit 1; }

{
  cat "$HERE/ruby2d_shim.rb"
  cat <<'BODY'

# rbtris uses a Mutex to keep its timer thread and the render loop apart; here
# there is one thread and the host drives the frames, so a no-op will do.
unless defined?(Mutex)
  class Mutex
    def synchronize = yield
  end
end

# The high score lives in ~/.rbtris.  wasm has no HOME and no writable home, so
# point it somewhere harmless; the file simply never exists.
class Dir
  def self.home = "/nonexistent"
end
BODY
  # Drop the font download (lines up to `require "ruby2d"`) and keep the rest.
  awk 'f { print } /^require "ruby2d"$/ { f = 1 }' "$APP/main.rb"
  cat <<'BODY'

# --- interactive driver ------------------------------------------------------
# Protocol: one byte in / one frame out.
#   in   bit 0 left, 1 right, 2 up, 3 down, 4 space, 5 r, 0xff = quit
#   out  Window.width * Window.height * 3 bytes, RGB
KEYS = ['left', 'right', 'up', 'down', 'space', 'r'].freeze
W = Ruby2D::Window.width
H = Ruby2D::Window.height
fb = Array.new(W * H * 3, 0)

$stdout.write("PAL0")
$stdout.write(([0] * 768).pack("C*"))

held = 0
loop do
  b = STDIN.read(1)
  break if b.nil?
  v = b.unpack1("C")
  break if v == 0xff

  KEYS.each_with_index do |name, i|
    now = (v >> i) & 1
    was = (held >> i) & 1
    ev = KeyEvent.new(name)
    if now == 1 && was == 0
      (Ruby2D::Window.handlers[:key_down] || []).each { |h| h.call(ev) }
    elsif now == 1
      (Ruby2D::Window.handlers[:key_held] || []).each { |h| h.call(ev) }
    elsif was == 1
      (Ruby2D::Window.handlers[:key_up] || []).each { |h| h.call(ev) }
    end
  end
  held = v

  Ruby2D::Window.run_update
  Ruby2D::Window.frame_done
  Ruby2D::Window.render(fb)
  $stdout.write(fb.pack("C*"))
  $stdout.flush
end
BODY
} > "$OUT"
echo "$OUT"
