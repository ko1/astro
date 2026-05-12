"""pystro stub for CPython internal `_testcapi` extension."""

# Minimal classes / functions referenced by stdlib tests (test_call,
# test_typing, etc.).  Tests that probe specific C-level behaviour
# will still fail, but at least the test files can import and the
# remaining (pure-Python) tests run.


class _Meth:
    def meth_varargs(self, *args):
        return (self,) + args
    def meth_varargs_keywords(self, *args, **kwargs):
        return ((self,) + args, kwargs)
    def meth_noargs(self):
        return (self,)
    def meth_o(self, arg):
        return (self, arg)
    def meth_fastcall(self, *args):
        return (self,) + args
    def meth_fastcall_keywords(self, *args, **kwargs):
        return ((self,) + args, kwargs)
    def __call__(self, *args, **kwargs):
        return args


class MethInstance(_Meth):
    pass


class MethClass:
    @classmethod
    def meth_varargs(cls, *args):
        return (cls,) + args
    @classmethod
    def meth_varargs_keywords(cls, *args, **kwargs):
        return ((cls,) + args, kwargs)
    @classmethod
    def meth_noargs(cls):
        return (cls,)
    @classmethod
    def meth_o(cls, arg):
        return (cls, arg)
    @classmethod
    def meth_fastcall(cls, *args):
        return (cls,) + args
    @classmethod
    def meth_fastcall_keywords(cls, *args, **kwargs):
        return ((cls,) + args, kwargs)


class MethStatic:
    @staticmethod
    def meth_varargs(*args):
        return args
    @staticmethod
    def meth_varargs_keywords(*args, **kwargs):
        return (args, kwargs)
    @staticmethod
    def meth_noargs():
        return ()
    @staticmethod
    def meth_o(arg):
        return arg
    @staticmethod
    def meth_fastcall(*args):
        return args
    @staticmethod
    def meth_fastcall_keywords(*args, **kwargs):
        return (args, kwargs)


def _test_long_long_and_overflow(*args, **kwargs):
    return 0


def get_recursion_depth():
    return 0


def type_get_version(cls):
    return 0


def type_assign_specific_version_unsafe(cls, version):
    pass


def type_assign_version(cls):
    return 0


def type_get_tp_alloc(cls):
    return None


def type_modified(cls):
    pass


def type_freeze(cls):
    pass


def get_feature_macros():
    return {}


def has_vectorcall_flag(cls):
    return False


def pyobject_fastcall(*args, **kwargs):
    raise NotImplementedError("pyobject_fastcall")


def pyobject_vectorcall(*args, **kwargs):
    raise NotImplementedError("pyobject_vectorcall")


def pyobject_fastcalldict(*args, **kwargs):
    raise NotImplementedError("pyobject_fastcalldict")


def make_vectorcall_class(*args, **kwargs):
    raise NotImplementedError("make_vectorcall_class")


def meth_varargs(*args, **kwargs):
    return args


def meth_varargs_keywords(*args, **kwargs):
    return args, kwargs


def meth_noargs():
    return None


def meth_fastcall(*args, **kwargs):
    return args, kwargs


def meth_fastcall_keywords(*args, **kwargs):
    return args, kwargs


def meth_o(obj):
    return obj


class LimitedVectorCallClass:
    pass


class MethodDescriptor:
    pass


class MethodDescriptorBase:
    pass


class MethodDescriptorDerived(MethodDescriptor):
    pass


class MethodDescriptorNopGet:
    pass


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
