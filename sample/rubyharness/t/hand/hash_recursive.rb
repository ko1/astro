# Array#hash / Object#hash on a recursive container terminates and equal
# recursive structures hash equal. vs ruby.
a = [1, 'two', 3.0]; 5.times { a << a }
b = [1, 'two', 3.0]; 5.times { b << b }
p a.hash.class
p a.hash == b.hash
e = []; e << e
p e.hash.class
h = {x: 1}; h[:self] = h
p h.hash.class
