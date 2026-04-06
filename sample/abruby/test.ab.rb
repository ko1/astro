
class Point
  def initialize x, y
    @x = x
    @y = y
  end

  def x = @x
  def y = @y
  def inspect = "(#{@x}, #{@y})"

  def +(other)
    Point.new(@x + other.x, @y + other.y)
  end
end

p0 = Point.new(1, 2)
p1 = Point.new(3, 4)

p p0
p p1
p p0 + p1

__END__
a = Foo.new
b = Foo.new



p Foo.new.add 1, 2


__END__
def fib(n)
  if n < 2
    n
  else
    fib(n - 1) + fib(n - 2)
  end
end

p(fib(34))
