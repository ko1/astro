# Enumerator.produce(initial) { |prev| ... } — infinite enumerator; StopIteration
# terminates; no block raises ArgumentError. vs ruby.
p Enumerator.produce(1) { |n| n * 2 }.first(5)
p Enumerator.produce(1) { |n| n + 1 }.take(4)
e = Enumerator.produce(0) { |n| n >= 3 ? raise(StopIteration) : n + 1 }
p e.to_a
p Enumerator.produce([0, 1]) { |a, b| [b, a + b] }.first(4)
begin; Enumerator.produce(1); rescue ArgumentError; p :noblock; end
p Enumerator.produce(1) { |n| n + 1 }.lazy.select(&:even?).first(3)
