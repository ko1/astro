def t; yield; rescue FrozenError; "FE"; end
p t { { a: 1 }.freeze.shift }
p t { { a: 1 }.freeze.keep_if { true } }
p t { { a: 1 }.freeze.delete_if { true } }
h = { a: 1, b: 2, c: 3 }
out = []
h.keep_if { |k, v| out << [k, v]; v > 1 }
p out
p h
