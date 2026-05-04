# list literal + indexing + methods
xs = [10, 20, 30, 40, 50]
print(xs)
print(xs[0])
print(xs[-1])
print(xs[1:4])
print(xs[::2])
print(xs[::-1])
print(len(xs))

xs.append(60)
print(xs)
xs[0] = 99
print(xs)

# tuple
t = (1, 2, 3)
print(t)
print(t[0] + t[1] + t[2])
print(len(t))

# dict
d = {"a": 1, "b": 2, "c": 3}
print(d["a"])
d["d"] = 4
print(len(d))
print("a" in d)
print("z" in d)
print(sorted(d.keys()))

# range + for
total = 0
for i in range(1, 11):
    total += i
print(total)

# range(start, stop, step)
print(list(range(0, 10, 2)))
print(list(range(10, 0, -1)))

# break/continue
out = []
for i in range(10):
    if i == 3:
        continue
    if i == 7:
        break
    out.append(i)
print(out)

# string indexing/slicing
s = "Hello, World!"
print(s[0])
print(s[-1])
print(s[7:12])
print(s[::-1])
print(len(s))

# string methods
print("  hello  ".strip())
print("a,b,c,d".split(","))
print("-".join(["x", "y", "z"]))
print("Hello".upper())
print("WORLD".lower())
print("foobar".find("bar"))
print("foobar".replace("bar", "baz"))
print("hello".startswith("he"))
print("hello".endswith("lo"))
