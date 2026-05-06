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


def get_type_hints(obj):
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
        nt = _collections.namedtuple(name, field_names) if field_names else None
        if nt is None:
            return type.__new__(mcs, name, bases, ns)
        # Apply defaults (right-aligned).
        if defaults:
            try:
                nt.__defaults__ = tuple(defaults[k] for k in field_names if k in defaults)
            except Exception:
                pass
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
