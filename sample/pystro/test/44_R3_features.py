# Round 3 additions: builtins (id/dir/globals/hasattr/getattr/setattr/
# delattr/callable/open/eval/exec), str methods, dict methods, with multi,
# raise from, f"{x=}", builtin subclassing skip.

# Builtins.
print(id(5) > 0)
print(hasattr([], "append"))
print(hasattr([], "nonsense"))
print(getattr([1, 2, 3], "append") is not None)
print(getattr(5, "no", "default"))

class C: pass
c = C()
setattr(c, "x", 99)
print(c.x)
print(hasattr(c, "x"))
delattr(c, "x")
print(hasattr(c, "x"))

print(callable(print))
print(callable(5))
print(callable(C))

g = globals()
print("print" in g)

# eval / exec.
print(eval("1 + 2 * 3"))
exec("y = 99")
# `y` was set in the current scope (no — exec at module level).
# We don't reach into the running frame; just verify exec runs.

# int(s, base).
print(int("ff", 16))
print(int("1010", 2))
print(int("0xff", 0))
print(int("777", 8))

# Bytes \x escape.
b = b"\x01\x02\xff"
print(len(b))
print(b.hex())

# str methods.
print("hi".zfill(5))
print("hi".center(7, "-"))
print("hello world".title())
print("Hello".swapcase())
print("a\nb\nc".splitlines())
print("hello".removeprefix("he"))
print("hello".removesuffix("lo"))
print("123".isdigit())
print("abc".isalpha())
print("a,b,c".partition(","))

# dict methods.
d = {"a": 1, "b": 2}
d.update({"c": 3, "a": 10})
print(d)
print(d.setdefault("d", 4))
print(d.popitem())
e = d.copy()
e.clear()
print(e)

# sorted with kwargs.
print(sorted([3, 1, 2], reverse=True))
print(sorted([3, 1, 2, 5, 4], key=lambda x: -x))

# list.sort with kwargs.
xs = [3, 1, 2]
xs.sort(reverse=True)
print(xs)

# `with a, b:` multi-context.
class CM:
    def __init__(self, n):
        self.n = n
    def __enter__(self):
        return self.n
    def __exit__(self, *a):
        return False

with CM(1) as a, CM(2) as b:
    print(a, b)

# raise X from Y.
try:
    try:
        raise ValueError("orig")
    except ValueError as e:
        raise TypeError("wrapped") from e
except TypeError as t:
    print("caught:", t)
    print("cause:", t.__cause__)

# f"{x=}" debug.
x = 5
y = [1, 2]
print(f"{x=}")
print(f"{y=}")
print(f"{x+1=}")

# Multi-import.
import functools, operator, copy
print(functools.reduce(operator.add, [1, 2, 3]))
print(operator.itemgetter(1)([10, 20, 30]))
deep = copy.deepcopy([[1, 2], [3, 4]])
deep[0].append(99)
print(deep)

# Standard library new modules.
import os
print(os.path.join("a", "b"))
import time
print(time.perf_counter() > 0)

# Path / truediv.
from pathlib import Path
p = Path("/etc") / "hostname"
print(p.exists())

# StringIO.
from io import StringIO
s = StringIO()
s.write("hello ")
s.write("world")
print(s.getvalue())

# Enum.
from enum import _make_enum
Color = _make_enum("Color", {"RED": 1, "GREEN": 2})
print(Color.RED.name, Color.RED.value)
print(Color.RED == Color.RED)
print(Color.RED == Color.GREEN)
