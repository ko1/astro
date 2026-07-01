# Lazy chains over an infinite Enumerator.new generator drive incrementally
# (no eager materialization / hang). vs ruby.
fib = Enumerator.new do |y|
  a, b = 0, 1
  loop { y << a; a, b = b, a + b }
end
p fib.lazy.select(&:even?).first(5)
p fib.lazy.map { |x| x * 2 }.first(5)
p fib.lazy.reject(&:even?).first(4)
p fib.lazy.select(&:even?).map { |x| x * 10 }.first(3)
p fib.lazy.filter_map { |x| x * 100 if x.odd? }.first(3)
p fib.lazy.take(6).to_a
p fib.lazy.map { |x| x + 1 }.take(5).to_a
p fib.lazy.take_while { |x| x < 30 }.to_a
p fib.lazy.drop(3).first(4)
p fib.lazy.drop_while { |x| x < 10 }.first(3)
p fib.lazy.select(&:even?).take(4).to_a
nat = Enumerator.new { |y| i = 1; loop { y << i; i += 1 } }
p nat.lazy.map { |x| x * x }.first(5)
p nat.lazy.map { |x| x * 2 }.select { |x| x % 3 == 0 }.first(3)
# plain (non-lazy) generator methods stay eager
g = Enumerator.new { |y| [3, 1, 2].each { |x| y << x } }
p g.map { |x| x * 2 }
p g.select(&:odd?)
p g.to_a
p g.sort
p g.first(2)
# lazy over Array/Range/cycle unaffected
p (1..Float::INFINITY).lazy.select(&:even?).first(4)
p [1, 2, 3, 4].lazy.filter_map { |x| x * 2 if x.even? }.to_a
