# Rich decorator tests.

# Simple function decorator.
def trace(fn):
    def wrapper(*args, **kw):
        print("calling fn")
        return fn(*args, **kw)
    return wrapper

@trace
def hello(x):
    return f"hello, {x}"

print(hello("world"))

# Decorator that counts calls (closure-based, no func attr).
COUNT_F = [0]
def counted(fn):
    def wrapper(*args, **kw):
        COUNT_F[0] += 1
        return fn(*args, **kw)
    return wrapper

@counted
def f(x): return x * 2

print(f(1), f(2), f(3))
print(COUNT_F[0])

# Decorator that memoizes.
def memo(fn):
    cache = {}
    def wrapper(x):
        if x in cache:
            return cache[x]
        v = fn(x)
        cache[x] = v
        return v
    return wrapper

@memo
def slow_fib(n):
    if n < 2: return n
    return slow_fib(n-1) + slow_fib(n-2)

print(slow_fib(20))
print(slow_fib(30))

# Stacked decorators.
def deco_a(fn):
    def w(*a):
        return "A(" + str(fn(*a)) + ")"
    return w

def deco_b(fn):
    def w(*a):
        return "B(" + str(fn(*a)) + ")"
    return w

@deco_a
@deco_b
def core(x):
    return x

print(core("x"))   # A(B(x))

# Decorator with arguments (factory).
def repeat(times):
    def deco(fn):
        def wrapper(*a, **k):
            r = None
            for _ in range(times):
                r = fn(*a, **k)
            return r
        return wrapper
    return deco

@repeat(3)
def shout(s):
    return s + "!"

print(shout("hi"))

# Method decorators.
class C:
    @staticmethod
    def st():
        return "st"
    @classmethod
    def cm(cls):
        return "cm"
    @property
    def computed(self):
        return 42

c = C()
print(C.st())
print(C.cm())
print(c.computed)
