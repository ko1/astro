# Nested generators and yield-from variants.

# Inner gen called eagerly inside outer.
def inner(n):
    for i in range(n):
        yield i

def outer(n):
    for v in inner(n):
        yield v * 10

print(list(outer(4)))

# Two parallel generators, alternating consumption.
def evens():
    i = 0
    while True:
        yield i
        i += 2

def odds():
    i = 1
    while True:
        yield i
        i += 2

e = evens()
o = odds()
for _ in range(5):
    print(next(e), next(o))

# Generator nested 3-deep.
def lvl1():
    yield 1
    yield 2

def lvl2():
    for v in lvl1():
        yield v + 10
    yield 99

def lvl3():
    for v in lvl2():
        yield v * 2

print(list(lvl3()))

# Generator that consumes another generator built lazily inside.
def takesum(g, n):
    s = 0
    for _ in range(n):
        s += next(g)
    return s

def naturals():
    i = 0
    while True:
        yield i
        i += 1

print(takesum(naturals(), 10))

# Pairs of generators sharing nothing.
def squares(n):
    for i in range(n):
        yield i * i

def cubes(n):
    for i in range(n):
        yield i * i * i

a = list(squares(5))
b = list(cubes(5))
print(a, b)

# Generator inside a generator inside a list comprehension.
def digits(n):
    if n == 0:
        yield 0
        return
    while n > 0:
        yield n % 10
        n //= 10

def sumdig(n):
    s = 0
    for d in digits(n):
        s += d
    return s

print([sumdig(n) for n in range(20)])

# A generator that raises after some yields — exception caught outside.
def bad():
    yield 1
    yield 2
    raise ValueError("boom")

g = bad()
print(next(g))
print(next(g))
try:
    print(next(g))
except ValueError as e:
    print("caught", e)

# Nested gen with try/except inside.
def with_try():
    try:
        yield 1
        raise RuntimeError("x")
    except RuntimeError:
        yield 99
    yield 100

print(list(with_try()))

# Two nested gens, both with try.
def inner_try():
    try:
        yield 1
        raise ValueError("inner")
    except ValueError:
        yield -1

def outer_try():
    for v in inner_try():
        yield v * 2

print(list(outer_try()))
