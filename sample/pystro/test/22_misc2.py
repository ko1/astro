# Type annotations (parsed and discarded)
def add(x: int, y: int = 5) -> int:
    return x + y

print(add(3))
print(add(3, 4))

n: int = 42
print(n)

# bare annotation acts as a no-op
s: str
s = "hello"
print(s)

# assert
def safe_div(a, b):
    assert b != 0, "divide by zero"
    return a / b

print(safe_div(10, 2))
try:
    safe_div(10, 0)
except AssertionError as e:
    print("caught:", e.message)

# del item from dict / list / set
d = {"a": 1, "b": 2, "c": 3}
del d["b"]
print(sorted(d.keys()))

xs = [1, 2, 3, 4, 5]
del xs[2]
print(xs)

s = {1, 2, 3}
del s[2]
print(sorted(s))
