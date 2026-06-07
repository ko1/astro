# L1: blocks, iterators, Enumerable, Proc/lambda
[1, 2, 3].each { |x| print x }
puts
p [1, 2, 3].map { |x| x * 2 }
p [1, 2, 3, 4].select { |x| x.even? }
p [1, 2, 3, 4].reject { |x| x.even? }
p [1, 2, 3, 4].find { |x| x > 2 }
p [1, 2, 3, 4].reduce(0) { |a, x| a + x }
p [1, 2, 3, 4].inject(:+)
p [1, 2, 3, 4].sum
p [1, 2, 3].each_with_index.map { |x, i| [i, x] }
p [1, 2, 3].each_with_object([]) { |x, acc| acc << x * x }
p [3, 1, 2].sort_by { |x| -x }
p [1, 2, 3, 4].partition { |x| x.even? }
p [1, 2, 3, 4].group_by { |x| x % 2 }
p [1, 2, 3].flat_map { |x| [x, x] }
p (1..5).map { |x| x ** 2 }
p [1, 2, 3].all? { |x| x > 0 }
p [1, 2, 3].any? { |x| x > 2 }
p [1, 2, 3].none? { |x| x > 5 }
p [1, 2, 3].count { |x| x.odd? }
p [1, 2, 3, 4].take_while { |x| x < 3 }
p [1, 2, 3, 4].drop_while { |x| x < 3 }
p [1, 2, 3].min_by { |x| -x }
p [1, 2, 3].max_by { |x| -x }
p [[1, 2], [3, 4]].map { |a, b| a + b }
p({ a: 1, b: 2 }.map { |k, v| "#{k}=#{v}" })
p({ a: 1, b: 2 }.select { |k, v| v > 1 })
p({ a: 1, b: 2 }.each_with_object([]) { |(k, v), acc| acc << k })

acc = []
3.times { |i| acc << i }
p acc

result = []
1.upto(3) { |i| result << i * 10 }
p result

sq = ->(x) { x * x }
p sq.call(5)
p sq.(6)
p sq[7]

add = lambda { |a, b| a + b }
p add.call(2, 3)
p add.lambda?

pr = proc { |a, b| (a || 0) + (b || 0) }
p pr.call(2)
p pr.lambda?

def counter
  count = 0
  -> { count += 1 }
end
c = counter
p c.call
p c.call
p c.call

p [1, 2, 3].map(&:to_s)
double = :upcase.to_proc
p double.call("hi")

p [1, 2, 3].each.to_a
p [1, 2, 3].each_with_index.to_a
enum = [10, 20, 30].each
p enum.next
p enum.next
