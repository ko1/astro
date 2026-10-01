# Hash#shift / select! / reject! / keep_if / delete_if / compact! keep lookups
# by a user #hash/#eql? key correct (the key hints move with the pairs).
class K
  attr_reader :v
  def initialize(v) = @v = v
  def hash = @v.hash
  def eql?(o) = o.is_a?(K) && o.v == @v
end

# LRU cache: array keys holding user objects, evicted with shift
lru = {}
200.times do |i|
  lru[[K.new(i), :x]] = i
  lru.shift if lru.size > 40
end
p lru.size
p lru[[K.new(199), :x]], lru[[K.new(160), :x]], lru[[K.new(159), :x]]
p lru.key?([K.new(170), :x])
lru[[K.new(170), :x]] = :over
p lru.size, lru[[K.new(170), :x]]

def build(n) = (0...n).to_h { |i| [[K.new(i)], i] }

h = build(50); h.select! { |k, v| v.odd? }
p h.size, h[[K.new(7)]], h[[K.new(8)]], h[[K.new(49)]]
h = build(50); h.reject! { |k, v| v % 3 == 0 }
p h.size, h[[K.new(3)]], h[[K.new(4)]], h[[K.new(47)]]
h = build(50); h.keep_if { |k, v| v > 20 }
p h.size, h[[K.new(20)]], h[[K.new(21)]]
h = build(50); h.delete_if { |k, v| v < 30 }
p h.size, h[[K.new(29)]], h[[K.new(30)]]
h = build(50); 10.times { |i| h[[K.new(i)]] = nil }; p h.compact!.size
p h[[K.new(5)]], h[[K.new(15)]], h.key?([K.new(5)])

# insertion after a compaction still indexes correctly
h = build(40); h.reject! { |k, v| v.even? }
40.upto(90) { |i| h[[K.new(i)]] = i }
p h.size, h[[K.new(1)]], h[[K.new(2)]], h[[K.new(90)]], h[[K.new(64)]]

# a key mutated after insertion stays unreachable across a shift (CRuby)
a = [K.new(1)]
h = { [K.new(0)] => 0, a => 1, [K.new(2)] => 2 }
a << 9
h.shift
p h[a], h[[K.new(1)]], h[[K.new(2)]], h.size
h.rehash
p h[a]

# plain keys with an index
h = (0...100).to_h { |i| ["s#{i}", i] }
30.times { h.shift }
p h.size, h["s29"], h["s30"], h["s99"]
h.select! { |k, v| v % 7 == 0 }
p h.size, h["s35"], h["s36"]
