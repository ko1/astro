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
while i < 500000
  pt = Point.new(i, i + 1)
  sum += pt.dist
  i += 1
end
p(sum)

__END__

def fact(n)
  if n < 2
    1
  else
    n * fact(n - 1)
  end
end

i = 0
while i < 100
  fact(50)
  i += 1
end
p(fact(50))

__END__

def f
  require_relative 'a.ab.rb'
end

f
g

__END__
require_relative 'a.ab.rb'
require_relative 'a.ab.rb'


__END__

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
