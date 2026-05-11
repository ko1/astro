"""pystro stub for CPython internal `_testcapi` extension."""

# Minimal classes / functions referenced by stdlib tests (test_call,
# test_typing, etc.).  Tests that probe specific C-level behaviour
# will still fail, but at least the test files can import and the
# remaining (pure-Python) tests run.


class _Meth:
    def meth(self, *args, **kwargs):
        return args
    def __call__(self, *args, **kwargs):
        return args


class MethInstance(_Meth):
    pass


class MethClass:
    @classmethod
    def cmeth(cls, *args, **kwargs):
        return args


class MethStatic:
    @staticmethod
    def smeth(*args, **kwargs):
        return args


def _test_long_long_and_overflow(*args, **kwargs):
    return 0


def get_recursion_depth():
    return 0


def raise_exception(*args, **kwargs):
    raise RuntimeError("test")


def set_errno(value):
    pass


def get_errno():
    return 0


# Common probes that return constants.
MIN_PY_SSIZE_T = -2**31
MAX_PY_SSIZE_T = 2**63 - 1
PY_SSIZE_T_MAX = MAX_PY_SSIZE_T
PY_SSIZE_T_MIN = MIN_PY_SSIZE_T
PY_VERSION_HEX = 0x030C0000   # 3.12.0


__all__ = ["MethInstance", "MethClass", "MethStatic",
           "raise_exception", "get_recursion_depth"]
