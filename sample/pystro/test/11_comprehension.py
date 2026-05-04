print([x*x for x in range(10)])
print([x for x in range(20) if x % 2 == 0])
print(sorted([x for x in range(50) if x % 3 == 0 and x % 5 != 0]))

# nested for
flat = [(x, y) for x in range(3) for y in range(3) if x != y]
print(sorted(flat))

# dict comprehension (sort keys for stable output)
d = {k: k*k for k in range(5)}
print(sorted(d.keys()))
print([d[k] for k in sorted(d.keys())])

# chained ifs
print([x for x in range(20) if x > 5 if x < 12])

# nested in function
def squares(n):
    return [i*i for i in range(n)]

print(squares(6))

# tuple target in comprehension
inverted = {v: k for k, v in [("a", 1), ("b", 2), ("c", 3)]}
print(sorted(inverted.keys()))
