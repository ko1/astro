# ivar-heavy: exercises the inline slot cache (hit/miss/grow/polymorphic)
class Point
  def initialize(x, y)
    @x = x
    @y = y
    @tag = "p"
  end
  def x = @x
  def y = @y
  def move(dx, dy); @x += dx; @y += dy; self; end
  def sum; @x + @y; end
  def tag = @tag
end

pts = (0...5).map { |i| Point.new(i, i * 2) }
pts.each { |p| p.move(10, 100) }
p pts.map { |p| p.sum }
p pts.map { |p| [p.x, p.y, p.tag] }

# many ivars, set in initialize order then read repeatedly
class Wide
  def initialize
    @a=1; @b=2; @c=3; @d=4; @e=5; @f=6; @g=7; @h=8
  end
  def total; @a+@b+@c+@d+@e+@f+@g+@h; end
  def bump; @a+=1; @h+=1; total; end
end
w = Wide.new
p w.total
3.times { p w.bump }

# lazy / conditional ivar (created on first access path) + nil-before-set
class Lazy
  def get; @memo ||= compute; end
  def compute; @count = (@count || 0) + 1; @count * 10; end
  def raw; @memo; end
end
l = Lazy.new
p l.raw      # nil (unset)
p l.get      # 10
p l.get      # 10 (memoized)
p l.raw      # 10

# polymorphic site: same method-less access shape across two classes
class A; def initialize; @v = 1; @w = 2; end; def v = @v; end
class B; def initialize; @v = 100; end; def v = @v; end
mix = [A.new, B.new, A.new, B.new]
p mix.map { |o| o.v }
