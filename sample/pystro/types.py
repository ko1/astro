# pystro stdlib `types` (minimal).

# Reference type objects via `type()` of representative values.
_dummy_func = lambda: None
def _dummy_gen(): yield 1

FunctionType = type(_dummy_func)
LambdaType = FunctionType
GeneratorType = type(_dummy_gen())
BuiltinFunctionType = type(print)
BuiltinMethodType = BuiltinFunctionType
MethodType = type([].append)   # bound method on list
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
ModuleType = type(__import__("os"))
NoneType = type(None)
EllipsisType = type(Ellipsis)
NotImplementedType = type(NotImplemented)
TracebackType = type(None)   # placeholder; pystro has no tb objects


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


def resolve_bases(bases):
    return tuple(bases)


# Coroutine / async
CoroutineType = FunctionType   # placeholder; pystro async is a stub
AsyncGeneratorType = GeneratorType
