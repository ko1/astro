module Comparable; def clamp(*) = :cmp; end
p 5.clamp(1, 3), 5.0.clamp(1, 2), "b".clamp("a", "c")
class Hash; def sum(*) = :hash_own; end
module Enumerable; def sum(*) = :enum; end
p({a: 1}.sum, Set[1].sum, (1..3).sum, [1, 2].sum)
module Enumerable; def min_by = :mb; end
p({a: 1}.min_by, (1..2).min_by { _1 })
p Set[1].method(:sum).owner, (1..3).method(:sum).owner
