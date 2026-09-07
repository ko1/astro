# A pure-Ruby stand-in for the part of Ruby2D that a small game touches.
#
# The real gem binds to SDL through a C extension, so it cannot load under
# koruby (no C extensions) or in wasm (no dlopen).  Nothing about the *API* is
# native, though: it is a retained list of shapes that a renderer walks.  So
# this keeps the shapes and rasterises them into a framebuffer, and the host
# drives frames and keys over stdin/stdout like the other pages here.
#
# Implemented: set, Window.update / on(:key_down, :key_held, :key_up),
# Window.width / height / frames, Rectangle, Square, Text, Font.path, show.
# Colours may be a CSS-ish name, "#rrggbb", or [r, g, b, a] floats.

module Ruby2D
  COLORS = {
    'black' => [0, 0, 0], 'white' => [255, 255, 255], 'gray' => [128, 128, 128],
    'grey' => [128, 128, 128], 'red' => [255, 0, 0], 'green' => [0, 128, 0],
    'blue' => [0, 0, 255], 'yellow' => [255, 255, 0], 'aqua' => [0, 255, 255],
    'orange' => [255, 165, 0], 'purple' => [128, 0, 128], 'brown' => [165, 42, 42],
    'fuchsia' => [255, 0, 255], 'lime' => [0, 255, 0], 'navy' => [0, 0, 128],
    'teal' => [0, 128, 128], 'olive' => [128, 128, 0], 'maroon' => [128, 0, 0],
    'silver' => [192, 192, 192], 'random' => [200, 200, 200]
  }.freeze

  # 5x7 cells in a 6x8 box: enough for the score and prompt text a game shows.
  FONT_W = 6
  FONT_H = 8
  GLYPHS = {
    ' ' => %w[00000 00000 00000 00000 00000 00000 00000],
    '0' => %w[01110 10001 10011 10101 11001 10001 01110],
    '1' => %w[00100 01100 00100 00100 00100 00100 01110],
    '2' => %w[01110 10001 00001 00010 00100 01000 11111],
    '3' => %w[11111 00010 00100 00010 00001 10001 01110],
    '4' => %w[00010 00110 01010 10010 11111 00010 00010],
    '5' => %w[11111 10000 11110 00001 00001 10001 01110],
    '6' => %w[00110 01000 10000 11110 10001 10001 01110],
    '7' => %w[11111 00001 00010 00100 01000 01000 01000],
    '8' => %w[01110 10001 10001 01110 10001 10001 01110],
    '9' => %w[01110 10001 10001 01111 00001 00010 01100],
    ':' => %w[00000 00100 00100 00000 00100 00100 00000],
    "'" => %w[00100 00100 00000 00000 00000 00000 00000],
    '-' => %w[00000 00000 00000 01110 00000 00000 00000],
    '.' => %w[00000 00000 00000 00000 00000 00110 00110],
    'A' => %w[01110 10001 10001 11111 10001 10001 10001],
    'B' => %w[11110 10001 11110 10001 10001 10001 11110],
    'C' => %w[01110 10001 10000 10000 10000 10001 01110],
    'D' => %w[11110 10001 10001 10001 10001 10001 11110],
    'E' => %w[11111 10000 11110 10000 10000 10000 11111],
    'F' => %w[11111 10000 11110 10000 10000 10000 10000],
    'G' => %w[01110 10001 10000 10111 10001 10001 01111],
    'H' => %w[10001 10001 11111 10001 10001 10001 10001],
    'I' => %w[01110 00100 00100 00100 00100 00100 01110],
    'L' => %w[10000 10000 10000 10000 10000 10000 11111],
    'M' => %w[10001 11011 10101 10101 10001 10001 10001],
    'N' => %w[10001 11001 10101 10011 10001 10001 10001],
    'O' => %w[01110 10001 10001 10001 10001 10001 01110],
    'P' => %w[11110 10001 10001 11110 10000 10000 10000],
    'R' => %w[11110 10001 10001 11110 10100 10010 10001],
    'S' => %w[01111 10000 10000 01110 00001 00001 11110],
    'T' => %w[11111 00100 00100 00100 00100 00100 00100],
    'U' => %w[10001 10001 10001 10001 10001 10001 01110],
    'V' => %w[10001 10001 10001 10001 10001 01010 00100],
    'W' => %w[10001 10001 10001 10101 10101 11011 10001],
    'Y' => %w[10001 10001 01010 00100 00100 00100 00100]
  }.freeze

  def self.color_of(c)
    case c
    when nil then [255, 255, 255, 1.0]
    when Array then [(c[0] * 255).to_i, (c[1] * 255).to_i, (c[2] * 255).to_i, c[3] || 1.0]
    when String
      if c.start_with?('#')
        [c[1, 2].to_i(16), c[3, 2].to_i(16), c[5, 2].to_i(16), 1.0]
      else
        rgb = COLORS[c.downcase] || [255, 255, 255]
        [rgb[0], rgb[1], rgb[2], 1.0]
      end
    else [255, 255, 255, 1.0]
    end
  end

  class Shape
    attr_accessor :x, :y, :z, :color
    def initialize(x: 0, y: 0, z: 0, color: nil, **_rest)
      @x = x; @y = y; @z = z; @color = color
      add
    end
    def add
      Window.shapes << self unless Window.shapes.include?(self)
      self
    end
    def remove
      Window.shapes.delete(self)
      self
    end
  end

  class Rectangle < Shape
    attr_accessor :width, :height
    def initialize(width: 0, height: 0, **rest)
      @width = width; @height = height
      super(**rest)
    end
    def draw(fb, w, h)
      r, g, b, a = Ruby2D.color_of(@color)
      Window.fill_rect(fb, w, h, @x.to_i, @y.to_i, @width.to_i, @height.to_i, r, g, b, a)
    end
  end

  class Square < Rectangle
    def initialize(size: 0, **rest)
      super(width: size, height: size, **rest)
    end
    def size = @width
  end

  module Font
    def self.path(name) = name
  end

  class Text < Shape
    attr_accessor :text, :size
    def initialize(text = '', size: 12, **rest)
      @text = text.to_s
      @size = size
      super(**rest)
    end
    def text=(v)
      @text = v.to_s
    end
    def scale = [(@size / 8), 1].max
    def width = @text.to_s.length * FONT_W * scale
    def height = FONT_H * scale
    def draw(fb, w, h)
      r, g, b, a = Ruby2D.color_of(@color || 'white')
      s = scale
      @text.to_s.each_char.with_index do |ch, i|
        rows = GLYPHS[ch] || GLYPHS[ch.upcase] || GLYPHS[' ']
        rows.each_with_index do |row, ry|
          row.each_char.with_index do |bit, rx|
            next if bit == '0'
            Window.fill_rect(fb, w, h, @x.to_i + (i * FONT_W + rx) * s, @y.to_i + ry * s,
                             s, s, r, g, b, a)
          end
        end
      end
    end
  end

  module Window
    @width = 640
    @height = 480
    @shapes = []
    @update = nil
    @handlers = {}
    @frames = 0
    @keys = {}

    class << self
      attr_reader :shapes, :handlers
      attr_accessor :width, :height, :title

      attr_accessor :shown
      def frames = @frames
      def set(opts = {})
        @width = opts[:width] if opts[:width]
        @height = opts[:height] if opts[:height]
        @title = opts[:title] if opts[:title]
      end
      def update(&block) = @update = block
      def on(event, &block) = (@handlers[event] ||= []) << block
      def run_update = @update&.call
      def frame_done = @frames += 1

      def fill_rect(fb, w, h, x, y, rw, rh, r, g, b, a)
        y0 = y < 0 ? 0 : y
        x0 = x < 0 ? 0 : x
        y1 = y + rh; y1 = h if y1 > h
        x1 = x + rw; x1 = w if x1 > w
        yy = y0
        while yy < y1
          base = (yy * w + x0) * 3
          xx = x0
          while xx < x1
            if a >= 1.0
              fb[base] = r; fb[base + 1] = g; fb[base + 2] = b
            else
              fb[base]     = (fb[base]     * (1 - a) + r * a).to_i
              fb[base + 1] = (fb[base + 1] * (1 - a) + g * a).to_i
              fb[base + 2] = (fb[base + 2] * (1 - a) + b * a).to_i
            end
            base += 3
            xx += 1
          end
          yy += 1
        end
      end

      # Draw every shape in z order into an RGB byte array.
      def render(fb)
        i = 0
        n = @width * @height * 3
        while i < n
          fb[i] = 0; fb[i + 1] = 0; fb[i + 2] = 0
          i += 3
        end
        @shapes.sort_by { |s| s.z.to_i }.each { |s| s.draw(fb, @width, @height) }
        fb
      end
    end
  end
end

# Ruby2D exposes everything at the top level.
include Ruby2D
def set(**opts) = Ruby2D::Window.set(opts)

# The game calls this last to hand control to the window; here the host owns
# the loop, so it only marks that setup is finished.
def show
  Ruby2D::Window.shown = true
end

class KeyEvent
  attr_reader :key
  def initialize(key) = @key = key
end
