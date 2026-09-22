# `#hash` on a container dispatches #hash on its elements (CRuby's rb_hash), so
# two containers holding value-equal objects hash alike.  And the identity hash
# is the object_id, not the address: a moving GC must not change it.
class K
  attr_reader :v
  def initialize(v) = @v = v
  def hash = v.hash
  def eql?(o) = o.is_a?(K) && o.v == v
  def ==(o) = eql?(o)
  def inspect = "K(#{v})"
end

a, b = K.new(1), K.new(1)
p [a.hash == b.hash, [a].hash == [b].hash, [[a]].hash == [[b]].hash]
p({x: a}.hash == {x: b}.hash)
p [[a, 2].hash == [b, 2].hash, [a].hash == [K.new(2)].hash]
p [a, b].uniq.size
p [a, b].group_by { |x| x }.values.map(&:size)
p({a => 1}[b])

# identity survives a GC
o = Object.new
h1 = o.hash
2000.times { Array.new(100) { "x" * 40 } }
p h1 == o.hash

# recursive containers still terminate and stay CRuby-shaped
r = []
r << r
p [r.hash == r.hash, [r].hash == r.hash]
h = {}
h[:self] = h
p h.hash == h.hash

p [1, [2, [3]]].hash == [1, [2, [3]]].hash
p [1.0, 2r, 3+0i, (1..2), :s, "t", nil, true].hash == [1.0, 2r, 3+0i, (1..2), :s, "t", nil, true].hash

# Integer(Complex) — a real Complex converts, an imaginary one is a RangeError
p [Integer(5+0i), sprintf("%d", 5+0i), sprintf("%x", 5+0i)]
begin
  Integer(5+1i)
rescue RangeError => e
  p e.message
end
p Integer(Complex(5, 2), exception: false)
