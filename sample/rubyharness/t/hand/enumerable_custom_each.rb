class Stack
  include Enumerable
  def initialize; @items = []; end
  def push(x); @items.push(x); self; end
  def each(&b); @items.each(&b); end
end
s = Stack.new; s.push(3).push(1).push(2)
p s.to_a
p s.sort
p s.map { |x| x * 2 }
p s.select(&:odd?)
p s.reject(&:odd?)
p s.max
p s.min
p s.include?(2)
p s.reduce(:+)
p s.count
p s.count(&:odd?)
p s.sum
p s.sum { |x| x * 10 }
p s.first(2)
p s.find { |x| x > 1 }
p s.detect { |x| x > 100 }
p s.group_by(&:odd?)
p s.partition(&:even?)
p s.minmax
p s.sort_by { |x| -x }
p s.each_with_index.to_a
p s.flat_map { |x| [x, x] }
p s.tally
p s.any? { |x| x > 2 }
p s.all? { |x| x > 0 }
p s.none? { |x| x > 10 }
p s.one? { |x| x == 2 }
p s.find_index { |x| x == 2 }
p s.filter_map { |x| x * 100 if x.even? }
p s.take_while { |x| x > 0 }
p s.drop_while { |x| x == 3 }
p s.min_by { |x| -x }
p s.max_by { |x| -x }
p s.to_h { |x| [x, x * x] }
p s.each_with_object([]) { |x, a| a << x * 2 }
p s.chunk_while { |a, b| true }.to_a
p s.each_slice(2).to_a
p s.each_cons(2).to_a
p s.zip([10, 20, 30])
