# default arguments
def greet(name, greeting="hello", punct="!"):
    return greeting + ", " + name + punct

print(greet("world"))
print(greet("world", "hi"))
print(greet("world", "hi", "."))

# lambda
sq = lambda x: x * x
print(sq(7))

add = lambda a, b: a + b
print(add(3, 4))

# higher-order
def apply(f, x):
    return f(x)

print(apply(lambda x: x + 100, 5))
print(apply(sq, 9))

# closures
def make_counter():
    count = 0
    def inc():
        nonlocal_should_work = count + 1
        return nonlocal_should_work
    return inc

# Without true nonlocal we can't truly capture; just test that an
# inner def reading outer name through globals works.
g = 0
def gset(x):
    global g
    g = x

gset(42)
print(g)

# augmented assignment
x = 10
x += 5
print(x)
x -= 2
print(x)
x *= 3
print(x)
x //= 4
print(x)
x %= 5
print(x)
x = 0xFF
x &= 0x0F
print(x)
x |= 0xF0
print(x)
x ^= 0x55
print(x)
x = 1
x <<= 4
print(x)
x >>= 2
print(x)
x = 2
x **= 8
print(x)

# tuple unpack
a, b = 1, 2
print(a, b)
a, b = b, a
print(a, b)

x, y, z = (10, 20, 30)
print(x, y, z)

# `is` / `is not`
print(None is None)
print(None is not None)

# conditional expression
print("pos" if 5 > 0 else "neg")
print("pos" if -3 > 0 else "neg")
