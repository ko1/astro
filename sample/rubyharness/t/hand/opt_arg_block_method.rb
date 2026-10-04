# methods with optional parameters that use the block, through each dispatch path, with and without a block
class A
  def o(a, b = a * 2) = block_given? ? yield(a, b) : [a, b]
  def d(a, b = (block_given? ? :blk_in_default : :none)) = [a, b]
  def y(a, b = yield(a)) = [a, b]
  def p2(a, b = 1, &blk) = [a, b, blk&.call(a)]
  def via_self(x) = o(x)
  def via_self_blk(x) = o(x, 9) { |a, b| a + b }
end
class B < A
  def o(a, b = 0) = [:b, super]
end
a = A.new
p a.o(1), a.o(1, 5), a.o(1) { |x, y| x + y }, a.o(1, 5) { |x, y| x * y }
p a.d(1), a.d(1) {}, a.d(1, 2)
p a.y(3) { _1 * 10 }, a.y(3, 4)
p a.p2(1), a.p2(1, 2), a.p2(1) { _1 + 100 }
p a.via_self(2), a.via_self_blk(2)
p B.new.o(1), B.new.o(1, 2), B.new.o(1) { |x, y| [x, y] }
p a.send(:o, 6), a.public_send(:o, 6, 7) { |x, y| x - y }, a.method(:o).call(8)
begin
  a.y(1)
rescue LocalJumpError => e
  p e.class
end
r = 3.times.map { |i| GC.start; a.o(i) { |x, y| x + y } }
p r
