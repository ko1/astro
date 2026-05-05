# Rich exception handling.

# Basic try/except.
try:
    raise ValueError("oops")
except ValueError as e:
    print("caught", e)

# Multiple except clauses.
def maybe(x):
    if x == 1: raise ValueError("v")
    if x == 2: raise TypeError("t")
    if x == 3: raise KeyError("k")
    return "ok"

for i in [0, 1, 2, 3]:
    try:
        r = maybe(i)
        print(r)
    except ValueError:
        print("caught v")
    except TypeError:
        print("caught t")
    except KeyError:
        print("caught k")

# except without name.
try:
    raise ValueError("x")
except ValueError:
    print("caught nameless")

# except Exception catches everything (in our hierarchy).
try:
    raise IndexError("idx")
except Exception:
    print("caught base")

# else clause runs when no exception.
try:
    x = 1
except Exception:
    print("hit except")
else:
    print("hit else")

# finally runs always.
def f(raise_it):
    try:
        if raise_it:
            raise ValueError("boom")
        return "no-raise-return"
    except ValueError:
        return "caught-return"
    finally:
        print("finally")

print(f(False))
print(f(True))

# Nested try/except.
try:
    try:
        raise ValueError("inner")
    except TypeError:
        print("won't catch")
except ValueError as e:
    print("outer caught", e)

# Re-raise.
def relayer():
    try:
        raise ValueError("original")
    except ValueError as e:
        raise

try:
    relayer()
except ValueError as e:
    print("got", e)

# raise from a function chain.
def deep():
    raise RuntimeError("deep error")
def mid():
    deep()
def top():
    mid()
try:
    top()
except RuntimeError as e:
    print("propagated", e)

# Exception class hierarchy via except.
class MyExc(Exception):
    pass
class SubExc(MyExc):
    pass

try:
    raise SubExc("specific")
except MyExc as e:
    print("parent caught", e)

try:
    raise MyExc("generic")
except SubExc:
    print("won't catch")
except MyExc as e:
    print("base caught", e)

# Finally with return.
def with_finally():
    try:
        return "try-return"
    finally:
        print("finally runs even with return")
print(with_finally())

# Multiple exceptions in tuple.
try:
    raise ValueError("v")
except (TypeError, ValueError):
    print("caught either")

try:
    raise TypeError("t")
except (TypeError, ValueError):
    print("caught either 2")

# Custom exception with attributes (skip super() init — pystro v0
# doesn't yet provide a synthetic Exception.__init__).
class MyError(Exception):
    def __init__(self, code, msg):
        self.code = code
        self.message = msg

try:
    raise MyError(500, "server fail")
except MyError as e:
    print("code", e.code)

# AssertionError.
try:
    assert 1 == 2, "math broken"
except AssertionError as e:
    print("assert msg:", e)

# ZeroDivision.
try:
    x = 1 / 0
except ZeroDivisionError:
    print("zd")

# AttributeError.
class Plain: pass
p = Plain()
try:
    print(p.nope)
except AttributeError:
    print("ae")

# IndexError.
try:
    print([1, 2][10])
except IndexError:
    print("ix")

# KeyError.
try:
    print({"a": 1}["b"])
except KeyError:
    print("ky")

# Multiple except with finally + else combined.
def all_clauses(x):
    try:
        if x:
            raise ValueError(f"x={x}")
        result = "okay"
    except ValueError as e:
        result = "exc"
    else:
        result = result + "+else"
    finally:
        print("clean")
    return result

print(all_clauses(0))
print(all_clauses(1))
