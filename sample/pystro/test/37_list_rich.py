# Rich list operations / slicing / methods.

# Construction.
print([])
print([1])
print([1, 2, 3])
print(list())
print(list(range(5)))
print(list("abc"))

# Indexing.
a = [10, 20, 30, 40, 50]
print(a[0])
print(a[-1])
print(a[2])
print(a[-3])

# Slicing.
print(a[:])
print(a[:3])
print(a[3:])
print(a[1:4])
print(a[::2])
print(a[::-1])
print(a[-3:-1])
print(a[100:])
print(a[:0])
print(a[10:5])

# Mutation.
b = [1, 2, 3]
b[0] = 100
print(b)
b[-1] = 999
print(b)

# Slice assignment.
c = [1, 2, 3, 4, 5]
c[1:3] = [20, 30, 40]
print(c)

# Append / insert / pop / remove.
d = [1, 2]
d.append(3)
print(d)
d.insert(0, 0)
print(d)
print(d.pop())
print(d)
d.remove(0)
print(d)

# extend.
e = [1, 2]
e.extend([3, 4])
print(e)

# index / count.
print([1, 2, 3, 2, 1].index(2))
print([1, 2, 3, 2, 1].count(2))

# sort / sorted.
print(sorted([3, 1, 2]))
print(sorted(["banana", "apple", "cherry"]))

# reverse / reversed.
f = [1, 2, 3]
f.reverse()
print(f)
print(list(reversed([1, 2, 3])))

# Concatenation.
print([1, 2] + [3, 4])
print([0] * 5)
print(5 * [0])

# Membership.
print(3 in [1, 2, 3])
print(99 in [1, 2, 3])
print(3 not in [1, 2, 3])

# len / sum / min / max.
print(len([1, 2, 3]))
print(sum([1, 2, 3, 4]))
print(min([5, 3, 1, 4, 2]))
print(max([5, 3, 1, 4, 2]))
print(min(1, 2, 3))
print(max(1, 2, 3))

# any / all.
print(any([0, 0, 1]))
print(any([0, 0, 0]))
print(all([1, 1, 1]))
print(all([1, 0, 1]))

# Nested.
m = [[1, 2], [3, 4], [5, 6]]
print(m[0])
print(m[1][1])
print([row[0] for row in m])
print([row[i] for row in m for i in range(len(row))])

# Comprehensions.
print([x * x for x in range(5)])
print([x for x in range(10) if x % 2 == 0])
print([(i, j) for i in range(3) for j in range(2)])

# Equality.
print([1, 2, 3] == [1, 2, 3])
print([1, 2] == [1, 2, 3])
print([] == [])

# zip / enumerate.
print(list(zip([1, 2, 3], ["a", "b", "c"])))
print(list(enumerate(["x", "y", "z"])))

# map / filter via list().
print(list(map(lambda x: x*2, [1, 2, 3])))
print(list(filter(lambda x: x > 2, [1, 2, 3, 4])))

# Aliasing.
x = [1, 2, 3]
y = x
y.append(4)
print(x)

# Copy via slice.
z = x[:]
z.append(99)
print(x)
print(z)

# Tuple unpacking from list.
p, q, r = [10, 20, 30]
print(p, q, r)
