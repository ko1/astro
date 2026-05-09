"""pystro stub for `_typing` (CPython C-side accelerator for typing)."""


def _idfunc(x):
    return x


class TypeVar:
    def __init__(self, name, *constraints, bound=None, covariant=False,
                 contravariant=False, infer_variance=False):
        self.__name__ = name
        self.__constraints__ = tuple(constraints)
        self.__bound__ = bound
        self.__covariant__ = covariant
        self.__contravariant__ = contravariant
        self.__infer_variance__ = infer_variance
    def __repr__(self):
        return self.__name__


class ParamSpec:
    def __init__(self, name, *, bound=None, covariant=False, contravariant=False):
        self.__name__ = name
        self.__bound__ = bound
        self.args = ParamSpecArgs(self)
        self.kwargs = ParamSpecKwargs(self)


class TypeVarTuple:
    def __init__(self, name):
        self.__name__ = name


class ParamSpecArgs:
    def __init__(self, origin):
        self.__origin__ = origin


class ParamSpecKwargs:
    def __init__(self, origin):
        self.__origin__ = origin


class TypeAliasType:
    def __init__(self, name, value, *, type_params=()):
        self.__name__ = name
        self.__value__ = value
        self.__type_params__ = tuple(type_params)


class Generic:
    """Stand-in for typing.Generic.  Subclassing works (no parameter
    tracking)."""
    pass


__all__ = [
    "_idfunc", "TypeVar", "ParamSpec", "TypeVarTuple",
    "ParamSpecArgs", "ParamSpecKwargs", "TypeAliasType", "Generic",
]
