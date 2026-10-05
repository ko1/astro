# One @x read reached by objects of several shapes (a reader shared by subclasses that add ivars in different
# orders, `def x = @x`, attr_reader), shapes without the ivar (nil, remembered), remove_instance_variable,
# a class's own @x through the same node, frozen and dup'd objects, and GC moving them in between
class Base
  attr_reader :a
  def b = @b
  def both = [@a, @b]
  def initialize(a, b) = (@a = a; @b = b)
end
class OrderAB < Base; end
class OrderBA < Base; def initialize(a, b) = (@b = b; @a = a); end
class Extra < Base; def initialize(a, b) = (@z = 0; super); end
class OnlyA < Base; def initialize(a, _) = @a = a; end
class Empty < Base; def initialize = nil; end
objs = [OrderAB.new(1, 2), OrderBA.new(3, 4), Extra.new(5, 6), OnlyA.new(7, 8), Empty.new, OrderAB.new(9, 10)]
4.times do |i|
  GC.start if i == 2
  p objs.map(&:a), objs.map(&:b), objs.map(&:both)
end
o = OrderBA.new(:x, :y)
p o.a, o.b
o.remove_instance_variable(:@a)
p o.a, o.b, o.both
o.instance_variable_set(:@a, :back)
p o.a, o.both
e = Empty.new
p e.a, e.b
e.instance_variable_set(:@b, :late)
p e.a, e.b, e.both
class Base; @a = :class_level; end
p Base.instance_eval { @a }, objs[0].a
r = [Base, objs[0], objs[1], Base].map { |x| x.instance_eval { @a } }
p r
f = OrderAB.new(1, 2).freeze
p f.a, f.dup.b, f.clone.both
3.times { p [objs[3], objs[4], objs[1], objs[3]].map { |x| [x.a, x.b] } }
