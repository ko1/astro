"""pystro stub for `_operator` (the C accelerator for `operator`)."""

from operator import (
    add, sub, mul, truediv, floordiv, mod,
    neg, pos, and_, or_, xor, lshift, rshift, invert,
    eq, ne, lt, le, gt, ge,
    not_, truth, is_, is_not, contains,
    getitem, setitem, delitem, length_hint, index,
    iadd, isub, imul, itruediv, ifloordiv, imod, ipow,
    iand, ior, ixor, ilshift, irshift,
    matmul, imatmul,
    attrgetter, itemgetter, methodcaller,
)
from operator import pow_ as pow
from operator import abs_ as abs


def indexOf(a, b):
    for i, v in enumerate(a):
        if v == b:
            return i
    raise ValueError("not found")


def countOf(a, b):
    return sum(1 for v in a if v == b)


def concat(a, b): return a + b
def iconcat(a, b): a += b; return a


__all__ = [
    "add", "sub", "mul", "truediv", "floordiv", "mod", "pow", "neg", "pos",
    "abs", "invert", "lshift", "rshift", "and_", "or_", "xor",
    "lt", "le", "eq", "ne", "gt", "ge",
    "not_", "truth", "is_", "is_not", "contains", "indexOf", "countOf",
    "getitem", "setitem", "delitem", "length_hint", "index",
    "iadd", "isub", "imul", "itruediv", "ifloordiv", "imod", "ipow",
    "ilshift", "irshift", "iand", "ior", "ixor",
    "matmul", "imatmul", "concat", "iconcat",
    "attrgetter", "itemgetter", "methodcaller",
]
