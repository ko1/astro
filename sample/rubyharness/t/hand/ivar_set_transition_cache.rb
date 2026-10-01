# An ivar write that ADDS the ivar (the shape transition an initialize makes) through the
# node's cache: same site with different orders, growing past the values capacity, frozen
# objects, remove_instance_variable, subclasses, instance_variables order.  Run under both and diff.
class Pt
  def initialize(a, b, order)
    if order
      @a = a; @b = b
    else
      @b = b; @a = a
    end
    @c = a + b
  end
  def grow = (@d = 1; @e = 2; @f = 3; @g = 4; self)
  attr_reader :a, :b, :c
end
ps = 6.times.map { |i| Pt.new(i, i * 2, i.even?) }
p ps.map { [_1.a, _1.b, _1.c, _1.instance_variables] }
p ps.first.grow.instance_variables, Pt.new(1, 2, true).grow.instance_variable_get(:@g)
q = Pt.new(1, 2, true)
q.freeze
begin; q.grow; rescue => e; p e.class; end
p q.instance_variables
r = Pt.new(3, 4, true)
r.remove_instance_variable(:@b)
p r.instance_variables
r.instance_variable_set(:@b, 9)
p r.instance_variables, r.b
class SubPt < Pt
  def initialize(*) = (@z = 0; super)
end
p SubPt.new(1, 1, false).instance_variables
p 3.times.map { Pt.new(_1, 0, true).grow.instance_variables.size }
