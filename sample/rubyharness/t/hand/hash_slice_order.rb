h = { a: 1, b: 2, c: 3, d: 4 }
p h.slice(:c, :a)
p h.slice(:c, :a).keys
p h.slice(:x, :b, :y)
p h.slice
p h.except(:b, :d)
p h.except(:b, :d).keys
p h.except(:nope)
