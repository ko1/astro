def squares(n):
    for i in range(n):
        yield i * i

for x in squares(5):
    print(x)
print(list(squares(3)))

def evens(n):
    for i in range(n):
        if i % 2 == 0:
            yield i
print(list(evens(10)))

# yield from
def chain(a, b):
    yield from a
    yield from b
print(list(chain([1, 2, 3], [10, 20])))

# yield in while
def collatz(n):
    while n != 1:
        yield n
        if n % 2 == 0:
            n = n // 2
        else:
            n = 3 * n + 1
    yield 1
print(list(collatz(6)))

# generator + comprehension
def fibs(n):
    a, b = 0, 1
    for _ in range(n):
        yield a
        a, b = b, a + b

print(list(fibs(10)))
print([x for x in fibs(8) if x > 2])

# generator returning multiple types
def mixed():
    yield 1
    yield "hello"
    yield [1, 2]

for x in mixed():
    print(x)
