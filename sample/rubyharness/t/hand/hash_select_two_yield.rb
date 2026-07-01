# Hash#select/reject/filter yield |key, value| as TWO values (a 1-param block
# gets the key), unlike each/map which gather the [k,v] pair. vs ruby.
h = {a: 1, b: 2, c: 3}
p h.select { |k, v| v > 1 }
p h.reject { |k, v| v > 1 }
p h.filter { |k, v| k == :b }
got = []
h.select { |x| got << x; true }
p got
gm = []
h.map { |x| gm << x }
p gm
gf = []
h.find { |x| gf << x; false }
p gf
