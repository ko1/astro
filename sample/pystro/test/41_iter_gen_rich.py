# Rich iterator + generator tests.

# Custom iterator class.
class CountUp:
    def __init__(self, n):
        self.n = n
        self.i = 0
    def __iter__(self):
        return self
    def __next__(self):
        if self.i >= self.n:
            raise StopIteration
        v = self.i
        self.i += 1
        return v

print(list(CountUp(5)))
print([x for x in CountUp(3)])
total = 0
for x in CountUp(10):
    total += x
print(total)

# Generator basics.
def gen(n):
    for i in range(n):
        yield i * 2

print(list(gen(5)))
print([x for x in gen(4)])

# Generator with state.
def fib():
    a, b = 0, 1
    while True:
        yield a
        a, b = b, a + b

g = fib()
out = []
for _ in range(10):
    out.append(next(g))
print(out)

# Generator return triggers StopIteration.
def early(n):
    for i in range(n):
        if i == 3:
            return
        yield i

print(list(early(10)))

# zip + generator.
def squares():
    i = 0
    while True:
        yield i * i
        i += 1

zipped = list(zip(range(5), squares()))
print(zipped)

# enumerate.
print(list(enumerate(["a", "b", "c"])))

# map / filter chain.
print(list(map(lambda x: x*x, range(5))))
print(list(filter(lambda x: x % 2 == 0, range(10))))

# reversed on list.
print(list(reversed([1, 2, 3, 4])))

# sum/min/max with iter.
print(sum(range(10)))
print(min(range(1, 6)))
print(max([3, 1, 4, 1, 5, 9, 2, 6]))

# Nested generators.
def chain(*iterables):
    for it in iterables:
        for x in it:
            yield x

print(list(chain([1, 2], [3, 4], [5])))

# Generator with try/except.
def safe(seq):
    for x in seq:
        try:
            yield 100 // x
        except ZeroDivisionError:
            yield None

print(list(safe([1, 2, 0, 4, 0])))

# Two iterators on same source.
def src():
    yield 1
    yield 2
    yield 3

a = src()
b = src()
print(next(a), next(b), next(a), next(b))   # independent

# All iter protocol pieces.
xs = [10, 20, 30]
it = iter(xs)
print(next(it))
print(next(it))
print(next(it))
try:
    print(next(it))
except StopIteration:
    print("done")

# next with default.
it2 = iter([1])
print(next(it2))
print(next(it2, "default"))

# Iterating over a string yields chars.
out = []
for ch in "xyz":
    out.append(ch)
print(out)

# Iterating over a dict yields keys (in insertion order).
d = {"a": 1, "b": 2, "c": 3}
print(list(d))
print([(k, d[k]) for k in d])

# Iterating over a range.
print(list(range(5)))
print(list(range(2, 8)))
print(list(range(0, 10, 2)))
print(list(range(10, 0, -2)))

# Sum / product via reduce-like manual.
def prod(it):
    r = 1
    for x in it:
        r *= x
    return r
print(prod([1, 2, 3, 4, 5]))
