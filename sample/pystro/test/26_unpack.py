# Tuple/list unpacking and starred targets.

# Plain unpack from list and tuple.
a, b, c = [1, 2, 3]
print(a, b, c)
x, y = (10, 20)
print(x, y)

# RHS evaluated once before assignment.
counter = [0]
def src():
    counter[0] += 1
    return [1, 2, 3]
p, q, r = src()
print(p, q, r, counter[0])

# Swap.
a, b = 1, 2
a, b = b, a
print(a, b)

# Three-way swap.
x, y, z = 1, 2, 3
x, y, z = z, x, y
print(x, y, z)

# Starred at head.
a, *rest = [1, 2, 3, 4, 5]
print(a, rest)

# Starred at tail.
*pre, b = [10, 20, 30]
print(pre, b)

# Starred in middle.
a, *mid, z = [1, 2, 3, 4, 5]
print(a, mid, z)

# Starred consumes empty.
a, *empty, z = [10, 20]
print(a, empty, z)

# Single starred consumes everything.
*everything, = [1, 2, 3]
print(everything)

# Nested unpack via iter (no parens).
for k, v in [(1, "a"), (2, "b"), (3, "c")]:
    print(k, v)

# Unpack in for over enumerate.
for i, ch in enumerate("xyz"):
    print(i, ch)

# Star with iterator (range, generator).
a, *rest = range(5)
print(a, rest)

def gen():
    yield 1
    yield 2
    yield 3
    yield 4
a, *rest, z = gen()
print(a, rest, z)

# Unpack tuple from func return.
def two():
    return 7, 8
a, b = two()
print(a, b)

# Unpack with global target.
g_a = 0
g_b = 0
def set_globals():
    global g_a, g_b
    g_a, g_b = 100, 200
set_globals()
print(g_a, g_b)

# Star + global.
g_list = []
def fill():
    global g_list
    head, *g_list = [1, 2, 3, 4]
    return head
print(fill(), g_list)

# Error: too few values.
try:
    a, b, c = [1, 2]
except ValueError as e:
    print("vs1")

# Error: too many values.
try:
    a, b = [1, 2, 3]
except ValueError as e:
    print("vs2")

# Error: starred but RHS too short.
try:
    a, *rest, b, c = [1]
except ValueError as e:
    print("vs3")
