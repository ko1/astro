# Rich closure / scope tests.

# Basic closure capture.
def make_adder(n):
    def add(x):
        return x + n
    return add

a5 = make_adder(5)
a10 = make_adder(10)
print(a5(3), a10(3))

# Multiple captures.
def make_pair_op(a, b):
    def add(): return a + b
    def sub(): return a - b
    def mul(): return a * b
    return add, sub, mul

add, sub, mul = make_pair_op(7, 3)
print(add(), sub(), mul())

# Late binding (Python's behavior — captures the *variable*).
fns = []
for i in range(5):
    def f():
        return i
    fns.append(f)
# All return 4 (final value of i).
print([fn() for fn in fns])

# Default-arg trick to capture i by value.
fns2 = []
for i in range(5):
    def g(j=i):
        return j
    fns2.append(g)
print([fn() for fn in fns2])

# Nested closures (3 levels).
def outer():
    x = 1
    def middle():
        y = 10
        def inner():
            return x + y
        return inner
    return middle()
print(outer()())

# Closure modifying captured via nonlocal — outer name is rebound.
def counter():
    n = 0
    def inc():
        nonlocal n
        n += 1
        return n
    def get():
        return n
    return inc, get

inc, get = counter()
print(inc())
print(inc())
print(inc())
print(get())

# Two distinct counters don't share state.
i1, _ = counter()
i2, _ = counter()
print(i1(), i1(), i2())   # 1, 2, 1

# Closure capturing a list — mutation visible.
def make_log():
    items = []
    def add(x):
        items.append(x)
    def get():
        return items
    return add, get

add, get = make_log()
add(1); add(2); add(3)
print(get())

# Function returned from a class method captures self correctly.
class Wrapper:
    def __init__(self, v):
        self.v = v
    def make_getter(self):
        return lambda: self.v

w = Wrapper(42)
g = w.make_getter()
print(g())
w.v = 99
print(g())   # 99 — captured self by reference

# Closure across module-level globals — global modifies don't leak.
GLOBAL = 0
def bump():
    global GLOBAL
    GLOBAL += 1
bump()
bump()
bump()
print(GLOBAL)

# Function in function-in-function with outer modify.
def lvl1():
    x = 100
    def lvl2():
        nonlocal x
        x += 1
        def lvl3():
            return x
        return lvl3
    return lvl2

f = lvl1()
g = f()
print(g())   # 101
print(f()())  # 102 (and 103 if invoked again)

# Lambda capturing by reference.
fs = [lambda x=i: x for i in range(3)]
print([f() for f in fs])
