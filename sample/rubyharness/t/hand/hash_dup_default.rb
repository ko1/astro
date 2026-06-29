h = Hash.new(99)
p h.dup["x"]
p h.dup.default
g = Hash.new { |hash, k| k * 2 }
p g.dup[5]
p h.clone["y"]
p({ a: 1 }.dup)
p({ a: 1, b: 2 }.dup.keys)
