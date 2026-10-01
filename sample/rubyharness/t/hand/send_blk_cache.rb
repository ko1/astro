# `recv.m(args) { block }`: break / next / return out of the block, C methods (each/map/
# each_with_index) and Ruby methods that yield, redefinition, several receiver classes at
# one site, private with self., method_missing, refinements.  Run under both and diff.
class Y
  def twice(a) = [yield(a), yield(a + 1)]
  def blk?(&b) = b.call(3)
  def bg = block_given?
end
class Z < Y
  def twice(a) = [:z, yield(a)]
end
def site(o) = o.twice(1) { |x| x * 10 }
y = Y.new; z = Z.new
p 3.times.map { site(y) }, site(z), site(y)
p y.blk? { _1 + 1 }, y.bg { }, y.bg
p [1, 2, 3].map { |x| x * 2 }, [1, 2, 3].each_with_index.map { |x, i| x + i }, { a: 1 }.map { |k, v| [k, v] }
p([1, 2, 3].each { |x| break x * 100 if x == 2 })
p([1, 2, 3].map { |x| next 0 if x == 2; x })
def ret_from_block = [1, 2, 3].each { |x| return x * 7 if x == 2 }
p ret_from_block
p y.twice(5) { |x| break :broke }
class Y
  def twice(a) = [:redef, yield(a)]
end
p site(y)
class P2
  def go = self.pv(2) { _1 * 3 }
  private def pv(x) = yield(x)
end
p P2.new.go
begin; P2.new.pv(1) { }; rescue NoMethodError => e; p e.message[/private method/]; end
class MM
  def method_missing(n, *a, &b) = [:mm, n, b.call(*a)]
  def respond_to_missing?(*) = true
end
p MM.new.anything(4) { _1 + 1 }
module Ref
  refine Array do
    def each_twice = map { yield _1 }.map { yield _1 }
  end
end
using Ref
p [1, 2].each_twice { _1 + 1 }
h = Hash.new { |hh, k| hh[k] = k * 2 }
p [1, 2].each_with_object([]) { |x, acc| acc << h[x] }
e = [1, 2, 3].each
p e.next, [4, 5].sum { _1 * 2 }
