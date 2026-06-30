class Stack; include Enumerable; def initialize(*i); @i=i; end; def each; @i.each { |x| yield x }; end; end
s = Stack.new(3, 1, 2, 4, 5)
p s.lazy.map { |x| x * 10 }.first(2)
p s.lazy.select(&:odd?).to_a
p s.lazy.map { |x| x * x }.force
p s.lazy.reject(&:even?).to_a
p s.lazy.filter_map { |x| x * 100 if x > 2 }.first(2)
p s.lazy.map { |x| x + 1 }.select(&:even?).first(2)
p s.lazy.take(3).to_a
p s.lazy.drop(2).to_a
p s.lazy.flat_map { |x| [x, -x] }.first(4)
class Numbers; include Enumerable; def each; (1..10).each { |n| yield n }; end; end
n = Numbers.new
p n.lazy.select(&:even?).map { |x| x ** 2 }.first(3)
p n.lazy.take_while { |x| x < 5 }.to_a
p [1,2,3].lazy.map { |x| x * 2 }.first(2)
p (1..Float::INFINITY).lazy.select(&:even?).first(3)
