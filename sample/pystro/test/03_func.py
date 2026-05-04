def add(a, b):
    return a + b

def mul3(a, b, c):
    return a * b * c

def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

def fact(n):
    r = 1
    while n > 1:
        r = r * n
        n = n - 1
    return r

print(add(3, 4))
print(mul3(2, 3, 4))
print(fib(10))
print(fib(15))
print(fact(10))
print(fact(0))

# function with no explicit return → None
def noret():
    x = 1
print(noret())
