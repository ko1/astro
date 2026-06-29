# Enumerable over an #each that yields multiple values: collection/search use the
# gathered element ([1,2]); user blocks get raw values (arity destructures).
class M
  include Enumerable
  def each
    yield 1, 2
    yield 3, 4, 5
    yield 6
  end
end
m = M.new
p m.to_a
p m.map { |x| x }
p m.map { |a, b| [a, b] }
p m.select { |x| true }
p m.reject { |x| false }
p m.find { |a, b| a == 3 }
p m.include?([1, 2])
p m.first(2)
p m.count
p m.sort_by { |a, b| -a }
p m.group_by { |a, b| a > 2 }
p m.partition { |a, b| a > 2 }
p m.take(2)
p m.all? { |a, b| (b || 0) >= a }
p m.reduce(0) { |acc, x| acc + x[0] }
out = []
m.each_with_index { |x, i| out << [x, i] }
p out
p m.zip([10, 20, 30])

# single-value must be unchanged
class S
  include Enumerable
  def each; [3, 1, 2].each { |v| yield v }; end
end
s = S.new
p s.map { |x| x * 2 }
p s.select(&:even?)
p s.min
p s.sort_by { |x| -x }
p s.reduce(:+)
p s.inject(100) { |a, x| a + x }
p s.to_a
p s.group_by(&:even?)
