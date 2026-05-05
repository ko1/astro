# Truly lazy generator: infinite source, take first N
def naturals():
    n = 1
    while True:
        yield n
        n += 1

def take(it, n):
    out = []
    for x in it:
        if len(out) >= n:
            break
        out.append(x)
    return out

print(take(naturals(), 5))

# Composing generators
def evens(src):
    for x in src:
        if x % 2 == 0:
            yield x

print(take(evens(naturals()), 5))

# next() with default
def short():
    yield 1
    yield 2

g = short()
print(next(g))
print(next(g))
print(next(g, "EOF"))
