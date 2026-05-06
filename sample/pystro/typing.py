# pystro stdlib `typing` (minimal — type hints are runtime no-ops).
#
# Pystro discards function/variable annotations at parse time, so
# all of these are best-effort placeholder objects.  Subscripting
# (`List[int]`) returns the original generic, since pystro doesn't
# enforce type parameters.

class _GenericAlias:
    def __init__(self, origin, name):
        self._origin = origin
        self._name = name
    def __getitem__(self, params):
        return self          # `List[int]` => List itself; type-check not enforced
    def __repr__(self):
        return self._name
    def __call__(self, *args, **kw):
        return self._origin(*args, **kw)


List       = _GenericAlias(list,       "typing.List")
Tuple      = _GenericAlias(tuple,      "typing.Tuple")
Dict       = _GenericAlias(dict,       "typing.Dict")
Set        = _GenericAlias(set,        "typing.Set")
FrozenSet  = _GenericAlias(frozenset,  "typing.FrozenSet")
Optional   = _GenericAlias(lambda x: x, "typing.Optional")
Union      = _GenericAlias(lambda x: x, "typing.Union")
def _noop_call(*a, **kw): return None
Callable   = _GenericAlias(_noop_call, "typing.Callable")
Iterable   = _GenericAlias(iter,       "typing.Iterable")
Iterator   = _GenericAlias(iter,       "typing.Iterator")
Generator  = _GenericAlias(iter,       "typing.Generator")
Any        = _GenericAlias(lambda x: x, "typing.Any")
NoReturn   = _GenericAlias(lambda: None, "typing.NoReturn")


# Type aliases.
def TypeVar(name, *constraints):
    return name


def NewType(name, base):
    return base


# Protocol / runtime_checkable / Final / ClassVar — all no-ops.
class Protocol: pass
def runtime_checkable(cls): return cls
Final     = _GenericAlias(lambda x: x, "typing.Final")
ClassVar  = _GenericAlias(lambda x: x, "typing.ClassVar")
Literal   = _GenericAlias(lambda x: x, "typing.Literal")
Annotated = _GenericAlias(lambda x: x, "typing.Annotated")
TypeAlias = _GenericAlias(lambda x: x, "typing.TypeAlias")


# Generic — at runtime, parameterising a Generic class returns the class
# itself.  Subclassing Generic[T] is also a no-op (T is just a name).
class Generic:
    def __class_getitem__(cls, item):
        return cls

# AnyStr is a typevar.
AnyStr = "AnyStr"
Self = "Self"
Hashable = _GenericAlias(lambda x: x, "typing.Hashable")
Sized    = _GenericAlias(lambda x: x, "typing.Sized")
Container = _GenericAlias(lambda x: x, "typing.Container")
Mapping  = _GenericAlias(dict, "typing.Mapping")
MutableMapping = _GenericAlias(dict, "typing.MutableMapping")
Sequence = _GenericAlias(list, "typing.Sequence")
MutableSequence = _GenericAlias(list, "typing.MutableSequence")
Type     = _GenericAlias(type, "typing.Type")
Awaitable = _GenericAlias(lambda x: x, "typing.Awaitable")
Coroutine = _GenericAlias(lambda x: x, "typing.Coroutine")
AsyncIterable = _GenericAlias(lambda x: x, "typing.AsyncIterable")
AsyncIterator = _GenericAlias(lambda x: x, "typing.AsyncIterator")


# TypedDict — runtime no-op; subclasses are just dicts at runtime.
class _TypedDictMeta(type):
    def __new__(mcs, name, bases, ns):
        cls = type.__new__(mcs, name, bases, ns)
        cls.__total__ = ns.get("__total__", True)
        return cls

    def __call__(cls, *args, **kwargs):
        # TypedDict("Name", **kwargs) builds a dict of those values.
        if not args and kwargs:
            return dict(kwargs)
        if len(args) == 1 and isinstance(args[0], dict) and not kwargs:
            return dict(args[0])
        return dict(*args, **kwargs)


class TypedDict(metaclass=_TypedDictMeta):
    pass


def get_type_hints(obj, globalns=None, localns=None, include_extras=False):
    """Return the __annotations__ dict for a function or class."""
    try:
        ann = obj.__annotations__
    except (AttributeError, TypeError):
        ann = {}
    if ann:
        return dict(ann)
    return {}


def cast(typ, val):
    return val


# NamedTuple — when a class subclasses typing.NamedTuple with class-level
# annotations, build a namedtuple-like class with __init__/__eq__/_fields.
import collections as _collections


class NamedTupleMeta(type):
    def __new__(mcs, name, bases, ns):
        if name == "NamedTuple":
            return type.__new__(mcs, name, bases, ns)
        # Collect annotated attributes as fields in declaration order.
        annot = ns.get("__annotations__") or {}
        field_names = list(annot.keys())
        defaults = {}
        for n in field_names:
            if n in ns:
                defaults[n] = ns.pop(n)
        # Build via collections.namedtuple, then layer user methods.
        # Defaults align right-most: pass right-aligned default values.
        if not field_names:
            return type.__new__(mcs, name, bases, ns)
        defaults_list = []
        for n in field_names:
            if n in defaults:
                defaults_list.append(defaults[n])
            else:
                defaults_list = []  # break: any non-default after a defaulted is invalid
        # Defaults must be right-aligned: take the suffix of field_names where
        # every name has a default.
        right_default_count = 0
        for n in reversed(field_names):
            if n in defaults:
                right_default_count += 1
            else:
                break
        if right_default_count:
            d_tuple = tuple(defaults[n] for n in field_names[-right_default_count:])
            nt = _collections.namedtuple(name, field_names, defaults=d_tuple)
        else:
            nt = _collections.namedtuple(name, field_names)
        # Copy any user-defined methods onto nt.
        for k, v in ns.items():
            if k.startswith("__") and k in ("__init__",): continue
            if callable(v):
                setattr(nt, k, v)
        return nt


class NamedTuple(metaclass=NamedTupleMeta):
    pass


__all__ = [
    "List", "Tuple", "Dict", "Set", "FrozenSet",
    "Optional", "Union", "Callable", "Iterable", "Iterator", "Generator",
    "Any", "NoReturn", "Protocol", "Final", "ClassVar",
    "TypeVar", "NewType", "runtime_checkable", "get_type_hints", "cast",
    "NamedTuple",
]
