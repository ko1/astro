# User-defined __hash__ / __eq__ for dict & set keys.

class Pt:
    def __init__(self, x, y):
        self.x = x
        self.y = y
    def __hash__(self):
        return self.x * 1000 + self.y
    def __eq__(self, other):
        return isinstance(other, Pt) and self.x == other.x and self.y == other.y
    def __repr__(self):
        return f"Pt({self.x},{self.y})"

# Dict: lookup by content-equal key works.
d = {}
d[Pt(1, 2)] = "alpha"
d[Pt(3, 4)] = "beta"
print(d[Pt(1, 2)])
print(d[Pt(3, 4)])
print(Pt(1, 2) in d)
print(Pt(9, 9) in d)

# hash(obj) returns an int.
h1 = hash(Pt(5, 6))
print(h1, isinstance(h1, int))

# Equal objects have equal hashes (contract).
print(hash(Pt(1, 2)) == hash(Pt(1, 2)))

# Sets dedupe by hash+eq.
s = set()
s.add(Pt(1, 2))
s.add(Pt(1, 2))
s.add(Pt(3, 4))
s.add(Pt(1, 2))
print(len(s))

# Membership via __eq__.
print(Pt(1, 2) in s)
print(Pt(7, 7) in s)

# A small flurry of mixed inserts and lookups.
many = {}
for i in range(20):
    many[Pt(i, i)] = i * 10
print(sum(many.values()))
print(many[Pt(7, 7)])

# A class without __hash__ (default identity-based hash) — different
# instances are different keys.
class N:
    def __init__(self, v): self.v = v
a = N(1); b = N(1)
d3 = {a: "x", b: "y"}
print(len(d3))                  # 2 — distinct identities
print(d3[a], d3[b])

# __hash__ returning negative int.
class Neg:
    def __init__(self, v): self.v = v
    def __hash__(self): return -self.v
    def __eq__(self, o): return isinstance(o, Neg) and self.v == o.v
n_d = {Neg(5): "five", Neg(7): "seven"}
print(n_d[Neg(5)])
print(n_d[Neg(7)])

# bool(__hash__) sanity — using object as set key + iter.
sset = set()
for i in range(5):
    sset.add(Pt(i, i))
total = 0
for p in sset:
    total += p.x + p.y
print(total)
