# L1: modules, mixins, Comparable/Enumerable
module Greetable
  def greet
    "hi, #{name}"
  end
end

class Person
  include Greetable
  attr_reader :name
  def initialize(name)
    @name = name
  end
end

p Person.new("ada").greet
p Person.include?(Greetable)
p Person.ancestors.include?(Greetable)

module Sized
  def big?
    size > 10
  end
end

class Box
  include Sized
  def initialize(n)
    @n = n
  end
  def size
    @n
  end
end
p Box.new(5).big?
p Box.new(20).big?

class Temp
  include Comparable
  attr_reader :deg
  def initialize(d)
    @deg = d
  end
  def <=>(other)
    deg <=> other.deg
  end
end
p Temp.new(10) < Temp.new(20)
p Temp.new(30) > Temp.new(20)
p [Temp.new(3), Temp.new(1), Temp.new(2)].sort.map(&:deg)
p Temp.new(5).clamp(Temp.new(1), Temp.new(3)).deg
p [Temp.new(3), Temp.new(1)].min.deg

class NumberList
  include Enumerable
  def initialize(*nums)
    @nums = nums
  end
  def each
    @nums.each { |n| yield n }
  end
end
nl = NumberList.new(1, 2, 3, 4)
p nl.map { |x| x * 2 }
p nl.select { |x| x.even? }
p nl.to_a
p nl.include?(3)
p nl.min
p nl.max
p nl.sum
p nl.sort
p nl.first(2)
p nl.reduce(:+)

module Constants
  MAX = 100
  module_function
  def helper
    "helped"
  end
end
p Constants::MAX
p Constants.helper

module Outer
  module Inner
    VALUE = 42
  end
end
p Outer::Inner::VALUE
