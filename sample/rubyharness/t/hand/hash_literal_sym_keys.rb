# {sym: v, ...} with distinct Symbol-literal keys appends without the duplicate search; every other shape keeps CRuby's results
i = 3
h = { a: i, b: i + 1, c: [i] }
p h, h[:b], h.size, h.keys, h.key?(:c)
h[:d] = 9; h[:a] = 0
p h, h.to_a.last
p({ "a" => 1, a: 2 }, { a: 1, "a" => 2 }, { 1 => :x, a: :y })
k = :a
p({ k => 1, a: 2 }, { a: 1, k => 2 })
x = { z: 0 }
p({ a: 1, **x }, { **x, a: 1 }, { a: 1, **x, b: 2 })
big15 = { k1: 1, k2: 2, k3: 3, k4: 4, k5: 5, k6: 6, k7: 7, k8: 8, k9: 9, k10: 10, k11: 11, k12: 12, k13: 13, k14: 14, k15: 15 }
big17 = { k1: 1, k2: 2, k3: 3, k4: 4, k5: 5, k6: 6, k7: 7, k8: 8, k9: 9, k10: 10, k11: 11, k12: 12, k13: 13, k14: 14, k15: 15, k16: 16, k17: 17 }
p big15.size, big15[:k15], big17.size, big17[:k16], big17[:k17], big17.key(17)
big15[:new] = 1; big15[:k3] = 33
p big15.size, big15[:k3], big15[:new]
order = []
p({ x: (order << 1; 1), y: (order << 2; 2), z: (order << 3; 3) }, order)
g = { s: Array.new(200) { "q" * 40 }.size, t: (GC.start; :after), u: "x" * 3 }
p g
p({ é: 1, "ü": 2 }, { é: 1 }.keys.first.encoding)
e = {}
p e, {a: nil}, {a: nil}.key?(:a)
def kw(**o) = o
p kw(a: 1, b: 2), kw(**{ a: 1 }, b: 3)
3.times { |n| GC.start if n == 1; p({ n: n, m: n * 2 }) }
f = { a: 1 }.freeze
p f.frozen?, f.merge(b: 2), { a: 1 }.compare_by_identity.tap { |z| z[:a] = 5 }
