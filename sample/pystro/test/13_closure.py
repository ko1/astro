# Read-only closure (no nonlocal needed)
def adder(x):
    def add(y):
        return x + y
    return add

a3 = adder(3)
print(a3(10))
print(a3(100))

# Closure of two outer params
def mk(a, b):
    def f(c):
        return a + b * c
    return f

g = mk(10, 20)
print(g(0))
print(g(5))

# nonlocal write
def make_counter():
    n = 0
    def step():
        nonlocal n
        n += 1
        return n
    return step

c1 = make_counter()
c2 = make_counter()
print(c1())
print(c1())
print(c1())
print(c2())   # independent
print(c1())

# Decorator using closure
def trace(f):
    def wrapped(x):
        return f(x) * 1000 + x
    return wrapped

@trace
def square(n):
    return n * n

print(square(5))   # 25 * 1000 + 5

# Stacked decorators
def add_one(f):
    def w(x):
        return f(x) + 1
    return w

def double(f):
    def w(x):
        return f(x) * 2
    return w

@add_one
@double
def base(x):
    return x

print(base(5))    # double then add_one: (5*2)+1 = 11

# Three-level nesting
def outer(a):
    def mid(b):
        def inner(c):
            return a + b + c
        return inner
    return mid

print(outer(1)(2)(3))

# Closure capture within for-loop (Python's late-binding subtlety)
def mk_funcs():
    fs = []
    for i in range(3):
        def f(x, i=i):    # default-arg trick to capture i eagerly
            return x + i
        fs.append(f)
    return fs

fs = mk_funcs()
print(fs[0](10))
print(fs[1](10))
print(fs[2](10))
