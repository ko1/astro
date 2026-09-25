# Objects as Hash keys / Set members: identity keys, user #hash/#eql?, Struct
# keys, deletes between lookups, a key mutated after insertion, and #object_id
# staying put across GC while unrelated objects come and go.
objs = Array.new(300) { Object.new }
h = {}
objs.each_with_index { |o, i| h[o] = i }
p h.size, objs.all? { |o| h[o] == objs.index(o) }, h[Object.new]
objs.each_slice(3) { |a, _, _| h.delete(a) }
p h.size, h.key?(objs[0]), h[objs[1]], h[objs[299]]

S = Struct.new(:a, :b)
sh = {}
200.times { |i| sh[S.new(i, i.to_s)] = i }
p sh[S.new(150, "150")], sh[S.new(150, "x")], sh.size
100.times { |i| sh.delete(S.new(i * 2, (i * 2).to_s)) }
p sh.size, sh[S.new(151, "151")], sh[S.new(150, "150")]

class Pt
  attr_accessor :x
  def initialize(x) = @x = x
  def hash = @x.hash
  def eql?(o) = o.is_a?(Pt) && o.x == @x
end
pts = Array.new(100) { |i| Pt.new(i) }
set = Set.new(pts)
p set.include?(Pt.new(42)), set.include?(Pt.new(100)), set.size
pts[42].x = 1000
p set.include?(Pt.new(42)), set.include?(Pt.new(1000)), set.include?(pts[42])
set.reset
p set.include?(Pt.new(1000)), set.size

ids = objs.first(50).map(&:object_id)
20.times { Array.new(2000) { Object.new.tap(&:object_id) } }
GC.start
p objs.first(50).map(&:object_id) == ids
p objs.first(50).map(&:hash) == objs.first(50).map(&:hash)
