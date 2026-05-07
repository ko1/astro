"""pystro stub for `_opcode` (CPython bytecode introspection)."""

stack_effect = lambda *a, **k: 0


def is_valid(op):
    return 0 <= op < 256

get_executor = lambda *a, **k: None
get_specialization_stats = lambda: {}

# A few commonly-referenced opcode names.
EXTENDED_ARG = 144
NOP = 9
RESUME = 151

def get_opmap():
    return {}

def get_metadata():
    return []


# Bytecode introspection helpers — pystro has no real bytecode, so
# these are no-op true/false/empty.
def has_arg(opcode):
    return False


def has_const(opcode):
    return False


def has_name(opcode):
    return False


def has_jump(opcode):
    return False


def has_free(opcode):
    return False


def has_local(opcode):
    return False


def has_exc(opcode):
    return False


def is_pseudo(opcode):
    return False


def get_pseudo_metadata():
    return []


def get_specialized_metadata():
    return {}


def get_intrinsic1_descs():
    return []


def get_intrinsic2_descs():
    return []


def get_executor_count():
    return 0


def get_nb_ops():
    return [
        ("NB_ADD", "+"),
        ("NB_SUBTRACT", "-"),
        ("NB_MULTIPLY", "*"),
        ("NB_TRUE_DIVIDE", "/"),
        ("NB_FLOOR_DIVIDE", "//"),
        ("NB_REMAINDER", "%"),
        ("NB_POWER", "**"),
        ("NB_AND", "&"),
        ("NB_OR", "|"),
        ("NB_XOR", "^"),
        ("NB_LSHIFT", "<<"),
        ("NB_RSHIFT", ">>"),
        ("NB_MATRIX_MULTIPLY", "@"),
    ]


def get_intrinsic_ops():
    return []


def get_special_method_names():
    return [
        "__init__", "__new__", "__del__", "__repr__", "__str__",
        "__bytes__", "__format__", "__lt__", "__le__", "__eq__",
        "__ne__", "__gt__", "__ge__", "__hash__", "__bool__",
        "__getattr__", "__getattribute__", "__setattr__", "__delattr__",
        "__dir__", "__call__", "__len__", "__length_hint__",
        "__getitem__", "__setitem__", "__delitem__", "__missing__",
        "__iter__", "__next__", "__reversed__", "__contains__",
        "__add__", "__sub__", "__mul__", "__truediv__", "__floordiv__",
        "__mod__", "__pow__", "__lshift__", "__rshift__",
        "__and__", "__or__", "__xor__", "__invert__",
        "__radd__", "__rsub__", "__rmul__", "__rtruediv__", "__rfloordiv__",
        "__neg__", "__pos__", "__abs__", "__round__", "__floor__",
        "__ceil__", "__trunc__", "__index__", "__int__", "__float__",
        "__complex__", "__enter__", "__exit__",
        "__aenter__", "__aexit__", "__await__", "__aiter__", "__anext__",
        "__matmul__", "__rmatmul__",
    ]

