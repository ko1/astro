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
MappingProxyType = type(type({}))   # placeholder (mappingproxy is dict in pystro)
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
