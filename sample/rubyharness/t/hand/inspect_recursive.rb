# inspect on self-referential Array/Hash prints [...] / {...} instead of
# recursing (was an exponential hang via depth limit). vs ruby.
a = [1, "two", 3.0]
a << a << a
p a.inspect
h = {x: 1}
h[:self] = h
p h.inspect
b = [1]; b << [b, 2]
p b
e = []; e << e
p e.inspect
