# pystro stdlib `operator` (minimal).

def add(a, b): return a + b
def sub(a, b): return a - b
def mul(a, b): return a * b
def truediv(a, b): return a / b
def floordiv(a, b): return a // b
def mod(a, b): return a % b
def pow_(a, b): return a ** b
def neg(a): return -a
def pos(a): return +a
def abs_(a): return abs(a)
def and_(a, b): return a & b
def or_(a, b): return a | b
def xor(a, b): return a ^ b
def lshift(a, b): return a << b
def rshift(a, b): return a >> b
def invert(a): return ~a

def eq(a, b): return a == b
def ne(a, b): return a != b
def lt(a, b): return a < b
def le(a, b): return a <= b
def gt(a, b): return a > b
def ge(a, b): return a >= b

def not_(a): return not a
def truth(a): return bool(a)
def is_(a, b): return a is b
def is_not(a, b): return a is not b
def contains(a, b): return b in a

def getitem(seq, key): return seq[key]
def setitem(seq, key, val): seq[key] = val

def itemgetter(*items):
    if len(items) == 1:
        idx = items[0]
        def g(obj): return obj[idx]
        return g
    def g(obj):
        return tuple(obj[i] for i in items)
    return g

def attrgetter(*names):
    def get_one(obj, name):
        for part in name.split("."):
            obj = getattr(obj, part)
        return obj
    if len(names) == 1:
        nm = names[0]
        def g(obj): return get_one(obj, nm)
        return g
    def g(obj):
        return tuple(get_one(obj, n) for n in names)
    return g

def methodcaller(name, *args):
    def g(obj):
        return getattr(obj, name)(*args)
    return g

# In-place operator wrappers.  Note: pystro returns the result; for
# mutable ops (like list.iadd) the caller needs to assign back.
def iadd(a, b):
    if hasattr(a, "__iadd__"): return a.__iadd__(b)
    return a + b
def isub(a, b):
    if hasattr(a, "__isub__"): return a.__isub__(b)
    return a - b
def imul(a, b):
    if hasattr(a, "__imul__"): return a.__imul__(b)
    return a * b
def itruediv(a, b):
    if hasattr(a, "__itruediv__"): return a.__itruediv__(b)
    return a / b
def ifloordiv(a, b):
    if hasattr(a, "__ifloordiv__"): return a.__ifloordiv__(b)
    return a // b
def imod(a, b):
    if hasattr(a, "__imod__"): return a.__imod__(b)
    return a % b
def ipow(a, b):
    if hasattr(a, "__ipow__"): return a.__ipow__(b)
    return a ** b
def iand(a, b):
    if hasattr(a, "__iand__"): return a.__iand__(b)
    return a & b
def ior(a, b):
    if hasattr(a, "__ior__"): return a.__ior__(b)
    return a | b
def ixor(a, b):
    if hasattr(a, "__ixor__"): return a.__ixor__(b)
    return a ^ b
def ilshift(a, b):
    if hasattr(a, "__ilshift__"): return a.__ilshift__(b)
    return a << b
def irshift(a, b):
    if hasattr(a, "__irshift__"): return a.__irshift__(b)
    return a >> b


def index(a):
    return a.__index__() if hasattr(a, "__index__") else int(a)


def length_hint(a, default=0):
    try:
        return len(a)
    except TypeError:
        return default


def matmul(a, b):
    # `@` operator not supported in pystro
    raise NotImplementedError("matmul not supported")


def imatmul(a, b):
    return matmul(a, b)


def delitem(seq, key):
    del seq[key]


def neg_(a): return -a
__neg__ = neg


# Aliases.
pow = pow_
abs = abs_
not_op = not_
sub = sub  # idempotent

__all__ = [
    "add", "sub", "mul", "truediv", "floordiv", "mod", "pow_", "pow",
    "neg", "pos", "abs_", "abs",
    "and_", "or_", "xor", "lshift", "rshift", "invert",
    "eq", "ne", "lt", "le", "gt", "ge",
    "not_", "truth", "is_", "is_not", "contains",
    "getitem", "setitem", "delitem",
    "itemgetter", "attrgetter", "methodcaller",
    "iadd", "isub", "imul", "itruediv", "ifloordiv", "imod", "ipow",
    "iand", "ior", "ixor", "ilshift", "irshift",
    "index", "length_hint",
]
