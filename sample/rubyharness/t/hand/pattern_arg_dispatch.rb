# `any?(pat)` / `grep(pat)` dispatch `pat === element`: a Proc, a Method and
# a Set all define #=== and none of them is a "user object".
require "set"
pr = ->(a) { a > 1 }
p [[1,2,3].any?(pr), [1].any?(pr), [1,2].all?(pr), [1,2].none?(pr), [1,2].one?(pr), [2,3].all?(pr)]
p [1,2,3].grep(pr)
p [1,2,3].grep_v(pr)
p((1..3).any?(pr))
p((1..4).grep(pr))
p((1..4).grep_v(pr))
p({a: 1, b: 2}.grep(->(pair) { pair[1] > 1 }))
m = 2.method(:==)
p [[1,2,3].grep(m), [1,2,3].any?(m), [1,2,3].grep(Set[2,3])]
p [[1,2,3].any?(Integer), [1,"a"].any?(String), [1,2].any?(2..3), [1,2].grep(2..3)]
p [1,2,3].each_entry.any?(pr)
p [1,2,3].lazy.grep(pr).to_a

# Set / Hash operands may be any enumerable, not just a Set or an Array
p Set.new([1]).merge(3..5).to_a
p Set.new([1]).merge({a: 1}).to_a
p Set.new([1]).merge([2], 7..8).to_a
p Set.new.merge([9, 10].each).to_a
p (Set.new([1]) | (3..4))
p [Set.new([1, 2]) & (2..3), Set.new([1, 2]) - (2..3), Set.new([1, 2]) ^ (2..3)]
p Set.new([1, 2]).subtract(1..1).to_a
begin; Set.new([1]).merge(5); rescue ArgumentError => e; p e.message; end

# Hash#values_at takes the same default path as #[]
h = Hash.new { |hh, k| hh[k] = k.to_s if k.is_a?(Symbol) }
p h.values_at(:a, :b)
p h
p Hash.new(:dflt).values_at(:x)
p({a: 1}.values_at(:a, :zz))
class HashWithDefault < Hash
  def default(k) = "d#{k}"
end
p HashWithDefault.new.values_at(:q)
