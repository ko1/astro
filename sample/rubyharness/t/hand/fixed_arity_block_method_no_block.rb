# fixed-arity methods that use the block, called with and without one, through each dispatch path
class A
  def g(x) = block_given? ? yield(x) : [:noblk, x]
  def h(x, &b) = [b.nil?, b ? b.call(x) : x]
  def y(x) = yield(x)
  def via_self(x) = g(x)
  def via_self_blk(x) = g(x) { _1 * 3 }
  def kw_last(x) = block_given? ? yield(x) : x
end
class B < A
  def g(x) = [:b, super]
  def h(x, &b) = super
end
a = A.new
p a.g(1), a.g(1) { _1 + 1 }, a.h(2), a.h(2) { _1 * 2 }
p a.via_self(4), a.via_self_blk(4)
p B.new.g(5), B.new.g(5) { _1 - 1 }, B.new.h(6), B.new.h(6) { _1 + 6 }
p a.send(:g, 7), a.public_send(:g, 7) { :blk }, a.method(:g).call(8)
begin
  a.y(1)
rescue LocalJumpError => e
  p e.class
end
p a.kw_last({ k: 1 }), a.kw_last(k: 2)
r = 3.times.map { |i| GC.start; a.g(i) }
p r
o = Object.new
def o.m(x) = block_given? ? :yes : :no
p o.m(1), o.m(1) {}
