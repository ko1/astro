# `%` operator and `str.format()` formatting.

# Basic types via %.
print("%d %s %f" % (42, "hi", 3.14))
print("%i %u" % (-5, 7))
print("%x %X %o" % (255, 255, 8))
print("%e %g" % (1234.5, 1234.5))

# Width / alignment / zero-pad.
print("|%10s|" % "hi")
print("|%-10s|" % "hi")
print("|%05d|" % 42)
print("|%+d|" % 42)
print("|%+d|" % -42)
print("|% d|" % 42)
print("|%.3f|" % 3.14159)

# Single-arg form (no tuple).
print("just %s" % "this")
print("num %d" % 5)

# %% literal (only meaningful in % formatting).
print("%d%% of %s" % (50, "tasks"))

# Multi-line format.
msg = "name=%s age=%d" % ("alice", 30)
print(msg)

# str.format() — basic.
print("{} {} {}".format(1, 2, 3))
print("{2} {1} {0}".format("a", "b", "c"))
print("{:>10}".format("right"))
print("{:<10}|".format("left"))
print("{:^10}|".format("ctr"))
print("{:0>5}".format(42))
print("{:.4f}".format(3.14159))
print("{:b}".format(10))
print("{:x}".format(255))

# !r conversion.
print("{!r}".format([1, 2]))
print("{!s}".format("hi"))

# Mixed positional + auto isn't allowed in CPython, but pystro
# permits one-or-the-other.  Test pure-positional.
print("{0}-{1}-{0}".format("x", "y"))

# {{ }} escapes.
print("{{}} {}".format("x"))

# Field with spec referencing positional index.
print("[{0:>8}]".format("ab"))
print("[{1:0>5d} / {0}]".format("hello", 42))

# Combine % and format in a list.
items = [(1, "a"), (2, "b"), (3, "c")]
for n, s in items:
    print("%d -> %s" % (n, s))
    print("{} -> {}".format(n, s))

# repr-ish via %r.
class P:
    def __repr__(self): return "<Pinst>"
print("%r %s" % (P(), P()))
