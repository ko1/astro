# The sorts whose comparisons run Ruby (a comparator block, <=> on user
# objects, sort_by with such keys, min(n)/max(n), Hash#sort) are merge sorts:
# same (stable) results as before, O(n log n), and correct when a comparison
# allocates enough to GC or raises.
V = Struct.new(:v, :tag) do
  include Comparable
  def <=>(o) = v <=> o.v
end
r = Random.new(7)
a = Array.new(3000) { r.rand(500) }
p a.sort { |x, y| y <=> x }.first(5), a.sort { |x, y| y <=> x } == a.sort.reverse
objs = a.each_with_index.map { |x, i| V.new(x, i) }
s = objs.sort
p s.first(3).map(&:to_a), s.each_cons(2).all? { |x, y| x.v < y.v || (x.v == y.v && x.tag < y.tag) }
p objs.sort_by { _1 }.first(3).map(&:to_a), objs.sort_by { _1 }.map(&:tag) == s.map(&:tag)
u = a.uniq.first(400)   # min(n)/max(n) order among equal elements is unspecified (CRuby's is not stable)
p a.max(5), a.min(5), u.max(3) { |x, y| (x * 7 % 401) <=> (y * 7 % 401) }, u.map { V.new(_1, 0) }.min(2).map(&:to_a)
h = a.each_with_index.to_h { |x, i| ["k#{i}", x] }
p h.sort.first(3), h.sort { |(k1, v1), (k2, v2)| v2 <=> v1 }.first(3)
p a.sort { |x, y| GC.start if x == 7; Array.new(50) { "g" }; x <=> y } == a.sort
p((a.sort { |x, y| raise "stop" if x == 99; x <=> y } rescue $!.message))
p (a.sort { |x, y| nil } rescue $!.class), ([1, "a"].sort rescue $!.class)
p [3, 1, 2].sort { |x, y| (x <=> y).to_f }, [3, 1, 2].sort_by! { -_1 }, [[2, :b], [1, :a], [2, :a]].sort_by(&:first)
