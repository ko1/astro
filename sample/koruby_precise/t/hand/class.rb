# L1: classes, instances, ivars, inheritance, super
class Point
  attr_accessor :x, :y
  def initialize(x, y)
    @x = x
    @y = y
  end

  def to_s
    "(#{@x}, #{@y})"
  end

  def +(other)
    Point.new(@x + other.x, @y + other.y)
  end

  def ==(other)
    @x == other.x && @y == other.y
  end
end

pt = Point.new(1, 2)
p pt.x
p pt.y
puts pt
pt.x = 10
p pt.x
q = Point.new(3, 4)
puts(pt + q)
p Point.new(1, 2) == Point.new(1, 2)
p pt.is_a?(Point)
p pt.instance_of?(Point)
p pt.respond_to?(:x)
p pt.class
p Point.instance_methods(false).sort

class Animal
  def initialize(name)
    @name = name
  end

  def speak
    "..."
  end

  def describe
    "#{@name} says #{speak}"
  end
end

class Dog < Animal
  def speak
    "woof"
  end
end

class Puppy < Dog
  def speak
    super + "!"
  end
end

p Dog.new("Rex").describe
p Puppy.new("Spot").describe
p Dog.superclass
p Dog.ancestors.include?(Animal)
p Puppy.new("x").is_a?(Animal)

class Counter
  @@total = 0
  def self.total
    @@total
  end
  def initialize
    @@total += 1
  end
end
Counter.new
Counter.new
p Counter.total

class WithConst
  PI = 3
  def pi
    PI
  end
end
p WithConst::PI
p WithConst.new.pi

class Builder
  def initialize
    @parts = []
  end
  def add(x)
    @parts << x
    self
  end
  def result
    @parts
  end
end
p Builder.new.add(1).add(2).add(3).result
