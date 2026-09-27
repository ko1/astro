# An Enumerator subclass builds a real enumerator through .new.
k = Class.new(Enumerator) { def extra = :x }
e = k.new { |y| y << 0; y << 1 }
p e.to_a, e.class == k, e.extra, e.next, e.size, e.map { _1 * 2 }
class Gen < Enumerator; def initialize(n) = super() { |y| n.times { y << _1 } }; end
p Gen.new(3).to_a, Gen.new(2).lazy.map { _1 + 1 }.to_a
