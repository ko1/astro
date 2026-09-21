# UnboundMethod#bind_call forwards its block like Method#call does (tilt binds a
# compiled template method and the template `yield`s), and `Object === x` is a
# real ancestor test -- a BasicObject is NOT an Object (tilt picks its scope
# path with `case scope when Object`).
class A
  def m(x) = [x, (block_given? ? yield(1) : :noblk)]
  def n(&b) = [:n, b ? b.call(2) : :noblk]
end
um = A.instance_method(:m)
p um.bind_call(A.new, 7) { |v| [:blk, v] }
p um.bind(A.new).call(7) { |v| [:blk2, v] }
p A.instance_method(:n).bind_call(A.new) { |v| [:blk3, v] }
p A.new.method(:m).call(8) { |v| [:blk4, v] }
p A.new.method(:m).unbind.bind_call(A.new, 9) { |v| [:blk5, v] }
pr = proc { |v| [:pr, v] }
p um.bind_call(A.new, 10, &pr)
p um.bind_call(A.new, 11, &nil)
# a builtin reached through bind_call still takes the block
p Array.instance_method(:map).bind_call([1, 2]) { |v| v * 3 }
p Kernel.instance_method(:tap).bind_call(5) { |v| v }

b = BasicObject.new
p [Object === b, BasicObject === b, Kernel === b]
p (case b when Object then :obj else :other end)
k = Class.new(BasicObject)
p [Object === k.new, BasicObject === k.new]
p Kernel.instance_method(:class).bind_call(b)
p [Object === 1, Object === nil, Object === Object, Object === self]
p [Object === Class.new.new, Comparable === 1, Enumerable === []]
