s = {1, 2, 3}
print(sorted(s))
s.add(4)
print(sorted(s))
print(2 in s)
print(99 in s)
print(len(s))

# discard / remove
s.discard(99)   # no error
s.remove(2)
print(sorted(s))

# set comprehension
sq = {x*x for x in range(5)}
print(sorted(sq))

# union / intersection / difference
a = {1, 2, 3}
b = {3, 4, 5}
print(sorted(a.union(b)))
print(sorted(a.intersection(b)))
print(sorted(a.difference(b)))

# set() builtin
print(sorted(set([1, 1, 2, 3, 3, 5])))
print(sorted(set("abracadabra")))

# empty {} → dict, set() → empty set
print(type({}))
print(type(set()))

# iteration
total = 0
for x in {10, 20, 30}:
    total += x
print(total)
