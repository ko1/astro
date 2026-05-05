# Dict insertion order (Python 3.7+ semantics).

# Plain insertion.
d = {}
d["zebra"] = 1
d["apple"] = 2
d["mango"] = 3
print(d)
print(list(d.keys()))
print(list(d.values()))
print(list(d.items()))
for k in d:
    print(k)

# Dict literal preserves order.
e = {"first": 1, "second": 2, "third": 3, "fourth": 4}
print(list(e.keys()))

# Update of existing key keeps original position.
d["apple"] = 200
print(d)

# Delete shifts nothing — others keep relative order.
del d["apple"]
print(list(d.keys()))

# Re-insertion of deleted key goes to end.
d["apple"] = 999
print(list(d.keys()))

# Mixed types.
m = {}
m[1] = "one"
m["two"] = 2
m[3.5] = "half"
m[(1, 2)] = "tup"
for k, v in m.items():
    print(k, v)

# Dict comprehension preserves source order.
src = ["a", "b", "c", "d"]
dc = {k: i for i, k in enumerate(src)}
print(list(dc.keys()))

# pop preserves order.
p = {"x": 1, "y": 2, "z": 3, "w": 4}
v = p.pop("y")
print(v)
print(list(p.keys()))

# Many inserts trigger resize but order is preserved.
big = {}
keys_in = []
for i in range(50):
    k = f"k_{i}"
    big[k] = i
    keys_in.append(k)
print(list(big.keys()) == keys_in)

# After lots of deletions and re-insertions order is well-defined.
g = {}
for i in range(10):
    g[i] = i * 10
del g[2]
del g[5]
del g[8]
print(list(g.keys()))
g[2] = 222
g[5] = 555
print(list(g.keys()))

# Nested dict displays in insertion order at all levels.
n = {"out_b": {"in_b": 1, "in_a": 2}, "out_a": {"in_y": 3, "in_x": 4}}
print(n)
