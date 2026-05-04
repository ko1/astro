def safe_div(a, b):
    try:
        return a / b
    except ZeroDivisionError as e:
        return None
    finally:
        print("done")

print(safe_div(10, 2))
print(safe_div(10, 0))

# custom exception class
class MyErr(Exception):
    pass

try:
    raise MyErr("oops")
except MyErr as e:
    print("caught:", e.message)

# fall-through to base class match
try:
    raise MyErr("base test")
except Exception as e:
    print("via Exception:", e.message)

# uncaught propagates from call
def bad():
    raise ValueError("bad")

try:
    bad()
except ValueError as e:
    print("caught:", e.message)

# multiple except clauses pick first match
def classify(x):
    try:
        if x == 0:
            raise ZeroDivisionError("zero")
        if x < 0:
            raise ValueError("neg")
        return "ok"
    except ZeroDivisionError:
        return "z"
    except ValueError:
        return "v"

print(classify(5))
print(classify(0))
print(classify(-1))

# implicit None re-raise from finally is normal flow
def with_finally(n):
    try:
        if n < 0:
            raise ValueError("neg")
        return n * 2
    finally:
        print("cleanup", n)

print(with_finally(3))
try:
    with_finally(-1)
except ValueError as e:
    print("got:", e.message)
