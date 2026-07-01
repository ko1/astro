# Enumerable#sum with a block gathers multi-yield into a single Array element.
# vs ruby.
class M
  include Enumerable
  def each; yield 1, 2; yield 3, 4, 5; yield 6; end
end
m = M.new
p m.sum { |e| e.is_a?(Array) ? e.size : e }
p m.sum(100) { |e| e.is_a?(Array) ? 1 : 0 }
# ordinary receivers unaffected
p [1, 2, 3, 4].sum
p [1, 2, 3, 4].sum { |x| x * 2 }
p (1..5).sum { |x| x ** 2 }
p [[1, 2], [3, 4]].sum([])
p ["a", "b", "c"].sum("")
