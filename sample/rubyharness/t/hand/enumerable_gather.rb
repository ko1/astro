# Enumerable#select/reject/find gather multi-yield into a single Array element
# for the block (which auto-splats by arity); map/flat_map spread instead. vs ruby.
class M
  include Enumerable
  def each; yield 1, 2; yield 3, 4, 5; yield 6; end
end
m = M.new
p m.select { |e| e.is_a?(Array) }
p m.reject { |e| e.is_a?(Array) }
p m.find { |e| e == [3, 4, 5] }
p m.select { |a, b| b.nil? }
p m.reject { |a, b| a == 3 }
p m.map { |e| e }
p m.map { |a, b| [a, b] }
p [1, 2, 3, 4].select(&:even?)
p [1, 2, 3, 4].reject(&:even?)
p [1, 2, 3].find { |x| x > 1 }
p({ a: 1, b: 2, c: 3 }.select { |k, v| v > 1 })
p({ a: 1, b: 2 }.find { |k, v| v == 2 })
