# `Klass.new(k: v)` binds the keywords straight into #initialize: required / optional / unknown / missing keywords,
# **rest, positional + keywords, super from initialize, initialize's return value ignored, a class with its own
# self.new, redefining initialize after the site cached it, GC between calls, and errors' messages
class Pt
  attr_reader :x, :y
  def initialize(x:, y: 2) = (@x = x; @y = y; :ignored)
end
class Pt3 < Pt
  def initialize(z, x:, **o) = (super(x: x, y: z); @o = o)
  attr_reader :o
end
class Own
  def self.new(**kw) = [:own, kw]
  def initialize(a:) = @a = a
end
class Later
  def initialize(v:) = @v = [:first, v]
  attr_reader :v
end
def mk(i) = Pt.new(x: i)
def mk2(i) = Pt.new(y: i, x: -i)
def mk3(i) = Pt3.new(i, x: i * 10, w: 1)
3.times do |r|
  GC.start if r == 1
  a = mk(r); b = mk2(r); c = mk3(r)
  p [a.class, a.x, a.y], [b.x, b.y], [c.class, c.x, c.y, c.o]
  p((Pt.new(y: 1) rescue [$!.class, $!.message]), (Pt.new(x: 1, q: 2) rescue [$!.class, $!.message]))
  p((Pt.new(1) rescue [$!.class, $!.message]), Own.new(a: r), Pt.new(**{ x: r }).x)
  p Later.new(v: r).v
  Later.class_eval { def initialize(v:) = @v = [:second, v] } if r == 1
end
