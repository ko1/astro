# keyword arguments
def f(a, b, c=10, d=20):
    return a + b * 100 + c * 10000 + d * 1000000

print(f(1, 2))
print(f(1, 2, 3))
print(f(1, 2, c=99))
print(f(1, 2, d=88, c=77))
print(f(b=2, a=1))

# *args
def sum_all(*args):
    s = 0
    for a in args:
        s += a
    return s

print(sum_all())
print(sum_all(1, 2, 3))
print(sum_all(1, 2, 3, 4, 5, 6, 7, 8, 9, 10))

# **kwargs
def kw_keys(**kw):
    return sorted(kw.keys())

print(kw_keys())
print(kw_keys(a=1, b=2, c=3))

# both
def both(*args, **kw):
    return [list(args), sorted(kw.keys())]

print(both(1, 2, 3))
print(both(1, x=2, y=3))
print(both(x=1))

# mix: pos + *args + kwonly + **kw
def h(a, b, *args, c=5, **kw):
    return [a, b, list(args), c, sorted(kw.keys())]

print(h(1, 2, 3, 4, 5))
print(h(1, 2, 3, c=100))
print(h(1, 2, c=99, x=10))
print(h(1, 2))

# default depending on outer var
GLOBAL = 100
def with_global_default(x=GLOBAL):
    return x
print(with_global_default())
print(with_global_default(42))

# forwarded kwargs via `*args` / `**kw` call-site unpacking
def caller(*args, **kw):
    return both(*args, **kw)
print(caller(1, 2, 3, x=99))
