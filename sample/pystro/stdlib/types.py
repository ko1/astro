# pystro stdlib `types` (minimal).

# Reference type objects via `type()` of representative values.
_dummy_func = lambda: None
def _dummy_gen(): yield 1

class CodeType:
    """Stub for `types.CodeType` — no compile target in pystro."""
    def __init__(self, *a, **kw):
        for i, n in enumerate(("co_argcount", "co_posonlyargcount",
                               "co_kwonlyargcount", "co_nlocals",
                               "co_stacksize", "co_flags", "co_code",
                               "co_consts", "co_names", "co_varnames",
                               "co_filename", "co_name", "co_qualname",
                               "co_firstlineno", "co_lnotab",
                               "co_exceptiontable", "co_freevars",
                               "co_cellvars")):
            setattr(self, n, a[i] if i < len(a) else kw.get(n, None))
    def replace(self, **kw):
        return self


class CellType:
    def __init__(self, value=None):
        self.cell_contents = value


class TracebackType:
    pass


class FrameType:
    pass


class ModuleType:
    def __init__(self, name, doc=None):
        self.__name__ = name
        self.__doc__ = doc
        self.__dict__ = {"__name__": name, "__doc__": doc}


class MethodWrapperType:
    pass


class WrapperDescriptorType:
    pass


class MemberDescriptorType:
    pass


class GetSetDescriptorType:
    pass


class ClassMethodDescriptorType:
    pass


class MethodDescriptorType:
    pass


class BuiltinMethodType_:
    pass


class AsyncGeneratorType:
    pass


class CoroutineType:
    pass


class EllipsisType:
    pass


class NoneType:
    pass


class NotImplementedType:
    pass


FunctionType = type(_dummy_func)
LambdaType = FunctionType
GeneratorType = type(_dummy_gen())
BuiltinFunctionType = type(print)
BuiltinMethodType = BuiltinFunctionType


# `MethodType(func, instance)` — bind func as a method of instance.
# Pystro's intrinsic bound-method type isn't constructible, so emulate
# it with a small callable class that holds (__func__, __self__).
# A custom metaclass with __instancecheck__ makes isinstance() also
# recognise pystro's intrinsic bound-method type (`type([].append)`),
# so existing code that does `isinstance(C().m, types.MethodType)`
# still works.
_BoundMethodType = type([].append)


class _MethodTypeMeta(type):
    def __instancecheck__(cls, instance):
        # First: pystro's intrinsic bound-method type (e.g. `[].append`,
        # or any `inst.method` of a user class).
        if type(instance) is _BoundMethodType:
            return True
        # Second: a real instance of MethodType (constructed via
        # `types.MethodType(func, inst)`).
        try:
            for base in type(instance).__mro__:
                if base is cls:
                    return True
        except Exception:
            pass
        return False


class MethodType(metaclass=_MethodTypeMeta):
    def __init__(self, func, instance):
        self.__func__ = func
        self.__self__ = instance
    def __call__(self, *args, **kwargs):
        return self.__func__(self.__self__, *args, **kwargs)
    def __repr__(self):
        return f"<bound method of {self.__self__!r}>"
    def __getattr__(self, name):
        return getattr(self.__func__, name)
    def __eq__(self, o):
        return (isinstance(o, MethodType)
                and self.__func__ is o.__func__
                and self.__self__ is o.__self__)
    def __hash__(self):
        return hash((id(self.__func__), id(self.__self__)))
# MappingProxyType — read-only view of a dict.  Pystro implements as a
# wrapper class that delegates lookups to an underlying dict and rejects
# mutations.
class MappingProxyType:
    def __init__(self, d):
        self._d = d
    def __getitem__(self, k):
        return self._d[k]
    def __contains__(self, k):
        return k in self._d
    def __len__(self):
        return len(self._d)
    def __iter__(self):
        return iter(self._d)
    def __repr__(self):
        return "mappingproxy(" + repr(self._d) + ")"
    def get(self, k, default=None):
        return self._d.get(k, default)
    def keys(self): return self._d.keys()
    def values(self): return self._d.values()
    def items(self): return self._d.items()
    def copy(self):
        return dict(self._d)
    def __eq__(self, o):
        if isinstance(o, MappingProxyType):
            return self._d == o._d
        return self._d == o
    def __ne__(self, o):
        return not self.__eq__(o)
    def __setitem__(self, k, v):
        raise TypeError("'mappingproxy' object does not support item assignment")
    def __delitem__(self, k):
        raise TypeError("'mappingproxy' object does not support item deletion")
    def __or__(self, o):
        if isinstance(o, MappingProxyType):
            return self._d | o._d
        return self._d | o
    def __ror__(self, o):
        return o | self._d
# Use `sys` to derive ModuleType — sys is built-in (pystro stub), no
# circular import.  Avoid `__import__("os")` because under PYTHONPATH=
# cpython/Lib it triggers CPython's os.py which has a deep dependency
# chain that may not be fully ready when pystro's types.py initializes.
import sys as _sys_for_modtype
ModuleType = type(_sys_for_modtype)
del _sys_for_modtype
NoneType = type(None)
EllipsisType = type(Ellipsis)
NotImplementedType = type(NotImplemented)
# Capture pystro's actual traceback / frame object types via a
# function-scoped raise.  Pystro only attaches __traceback__ when
# call_top > 0, so a module-level raise alone wouldn't produce
# anything.  This lets `isinstance(tb, TracebackType)` in
# inspect.istraceback / inspect.isframe etc. correctly identify
# pystro tracebacks (cf. cgitb / traceback inspection paths).
def _capture_types():
    global TracebackType, FrameType
    try:
        raise Exception("type-capture")
    except Exception as e:
        tb = getattr(e, "__traceback__", None)
        if tb is not None:
            TracebackType = type(tb)
            fr = getattr(tb, "tb_frame", None)
            if fr is not None:
                FrameType = type(fr)
TracebackType = type(None)
_capture_types()
del _capture_types


class SimpleNamespace:
    def __init__(self, **kwargs):
        for k, v in kwargs.items():
            setattr(self, k, v)
    def __repr__(self):
        items = []
        d = self.__dict__
        for k in sorted(d.keys()):
            items.append(k + "=" + repr(d[k]))
        return "namespace(" + ", ".join(items) + ")"
    def __eq__(self, o):
        if not isinstance(o, SimpleNamespace):
            return False
        return self.__dict__ == o.__dict__


def new_class(name, bases=(), kwds=None, exec_body=None):
    cls = type(name, bases, {})
    if exec_body is not None:
        ns = {}
        exec_body(ns)
        for k, v in ns.items():
            setattr(cls, k, v)
    return cls


def prepare_class(name, bases=(), kwds=None):
    return (type, {}, {})


# DynamicClassAttribute — descriptor used by `enum.property`.  Behaves
# like @property but lets subclasses override on the class itself.
class DynamicClassAttribute:
    def __init__(self, fget=None, fset=None, fdel=None, doc=None):
        self.fget = fget; self.fset = fset; self.fdel = fdel
        self.__doc__ = doc or (fget.__doc__ if fget else None)
        self.overwrite_doc = doc is None
        self.__isabstractmethod__ = False
    def __get__(self, instance, ownerclass=None):
        if instance is None: return self
        if self.fget is None: raise AttributeError("unreadable")
        return self.fget(instance)
    def __set__(self, instance, value):
        if self.fset is None: raise AttributeError("can't set")
        self.fset(instance, value)
    def __delete__(self, instance):
        if self.fdel is None: raise AttributeError("can't delete")
        self.fdel(instance)
    def getter(self, fget): return type(self)(fget, self.fset, self.fdel, self.__doc__)
    def setter(self, fset): return type(self)(self.fget, fset, self.fdel, self.__doc__)
    def deleter(self, fdel): return type(self)(self.fget, self.fset, fdel, self.__doc__)


# GenericAlias — backed by a simple wrapper.  PEP 585 returns this from
# `list[int]` / `dict[str, int]` etc.
class GenericAlias:
    def __init__(self, origin, args):
        self.__origin__ = origin
        self.__args__ = args if isinstance(args, tuple) else (args,)
    def __getitem__(self, params): return self
    def __call__(self, *a, **kw):
        # Calling `list[int](...)` constructs an instance of the origin.
        return self.__origin__(*a, **kw)
    def __eq__(self, other):
        if not isinstance(other, GenericAlias): return NotImplemented
        return (self.__origin__ is other.__origin__
                and self.__args__ == other.__args__)
    def __hash__(self):
        return hash((self.__origin__, self.__args__))
    def __repr__(self):
        # Note: comparing to Ellipsis via `is` failed in pystro across
        # module boundaries; use type().__name__ to detect ellipsis so
        # `tuple[int, ...]` renders the dots.
        def _fmt(a):
            if type(a).__name__ == "ellipsis": return "..."
            if isinstance(a, type):
                return a.__qualname__ if hasattr(a, "__qualname__") else a.__name__
            return repr(a)
        cls_name = (self.__origin__.__qualname__
                    if hasattr(self.__origin__, "__qualname__")
                    else self.__origin__.__name__)
        return cls_name + "[" + ", ".join(_fmt(a) for a in self.__args__) + "]"


# UnionType (PEP 604) — `int | str`.
class UnionType:
    def __init__(self, args):
        self.__args__ = args


def coroutine(func):
    return func


def resolve_bases(bases):
    return bases


def get_original_bases(cls):
    return getattr(cls, "__bases__", ())


def resolve_bases(bases):
    return tuple(bases)


# Coroutine / async
CoroutineType = FunctionType   # placeholder; pystro async is a stub
AsyncGeneratorType = GeneratorType
