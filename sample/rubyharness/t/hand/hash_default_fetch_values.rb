h = Hash.new { |hash, k| k * 2 }
p h[3]
h.default = 99
p h[3]
p h.default_proc
def t; yield; rescue FrozenError; "FE"; end
p t { {}.freeze.default = 1 }
g = {a: 1, b: 2}
p g.fetch_values(:a, :b)
p g.fetch_values(:a, :x) { |k| "missing:#{k}" }
def u; yield; rescue KeyError; "KE"; end
p u { g.fetch_values(:a, :z) }
