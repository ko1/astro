# A mutable String key is stored as a frozen private copy, so mutating the
# caller's String can't move the pair to another bucket.
s = "abc"
h = {}
h[s] = 1
p s.frozen?, h.keys.first.frozen?, h.keys.first.equal?(s)
s << "d"
p h["abc"], h["abcd"], h.keys

t = "xyz"
p({ t => 2 }.keys.first.frozen?)
u = "pqr"
v = {}
v.store(u, 3)
p v.keys.first.frozen?, v.keys.first.equal?(u)

fr = "fz".freeze
p({ fr => 1 }.keys.first.equal?(fr))

id = {}.compare_by_identity
k = "id"
id[k] = 1
p id.keys.first.equal?(k), id.keys.first.frozen?

class SubStr < String; end
ss = SubStr.new("sub")
hs = { ss => 1 }
p hs.keys.first.class, hs.keys.first.equal?(ss)

iv = "iv"
iv.instance_variable_set(:@x, 1)
hi = { iv => 1 }
p hi.keys.first.equal?(iv)

# value is untouched
val = "v"
hv = { :k => val }
p hv[:k].frozen?, hv[:k].equal?(val)

# replacing a value keeps the original key object
again = {}
kk = "k"
again[kk] = 1
first = again.keys.first
again[kk] = 2
p again.keys.first.equal?(first), again.size, again[kk]

# encoding survives the copy
e = "\xE3\x81\x82".dup.force_encoding("UTF-8")
p({ e => 1 }.keys.first.encoding.to_s)
b = "\xFF".dup.force_encoding("ASCII-8BIT")
p({ b => 1 }.keys.first.encoding.to_s, { b => 1 }.keys.first.bytes)
