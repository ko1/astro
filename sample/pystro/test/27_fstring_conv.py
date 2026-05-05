# f-string conversion suffixes !r / !s / !a (and combos with format spec).

s = "hi"
print(f"{s!s}")
print(f"{s!r}")
print(f"{s!a}")

# Container
x = [1, 2, 3]
print(f"{x!r}")
print(f"{x!s}")

# With format spec — alignment width applies to converted form.
print(f"|{s!r:>10}|")
print(f"|{s!r:<10}|")
print(f"|{s!r:^10}|")

# !r forces repr even for objects with custom __str__.
class C:
    def __str__(self):  return "STR"
    def __repr__(self): return "REPR"
c = C()
print(f"{c}")
print(f"{c!s}")
print(f"{c!r}")

# !s makes str() explicit.
print(f"int={42!s} repr={42!r}")

# Tuple.
t = (1, "two")
print(f"{t!r}")

# Dict.
d = {"a": 1}
print(f"{d!r}")

# Mixed in same f-string.
n = 7
print(f"{s!r}, {n}, {x!r}")

# Conversion + format spec on number.
print(f"{n!s:>5}")
print(f"{3.14!s:>8}")

# {{ and }} escapes still work.
print(f"{{lit {s!r} lit}}")
