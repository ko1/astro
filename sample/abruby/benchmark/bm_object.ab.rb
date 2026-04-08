# Object creation and method dispatch
class Point
  def initialize(x, y)
    @x = x
    @y = y
  end
  def x = @x
  def y = @y
  def dist
    @x * @x + @y * @y
  end
end

sum = 0
i = 0
while i < 3_500_000
  pt = Point.new(i, i + 1)
  sum += pt.dist
  i += 1
end
p(sum)
