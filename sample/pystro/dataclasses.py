# pystro stdlib `dataclasses` (minimal).
#
# `@dataclass` synthesizes __init__, __repr__, __eq__ on a class.
# pystro does not yet expose annotation reflection, so we have a small
# helper that requires you to declare fields explicitly OR pass them
# to `make_dataclass(...)`.
#
# Usage A (decorator + class attrs as defaults):
#   @dataclass
#   class P:
#       x = 0
#       y = 0
#       _fields = ("x", "y")           # explicit field order
#   p = P(1, 2); p.x; p.y; repr(p); P(1, 2) == P(1, 2)
#
# Usage B (factory):
#   P = make_dataclass("P", ["x", "y"])

def _build_dataclass(cls, fields):
    typename = cls.__name__ if hasattr(cls, "__name__") else "Dataclass"

    def _init(self, *args, **kwargs):
        if len(args) + len(kwargs) > len(fields):
            raise TypeError(typename + ".__init__: too many args")
        for i, fn in enumerate(fields):
            if i < len(args):
                setattr(self, fn, args[i])
            elif fn in kwargs:
                setattr(self, fn, kwargs[fn])
            elif hasattr(cls, fn):
                setattr(self, fn, getattr(cls, fn))
            # else: leave unset

    def _repr(self):
        parts = []
        for fn in fields:
            v = getattr(self, fn) if hasattr(self, fn) else None
            parts.append(fn + "=" + repr(v))
        return typename + "(" + ", ".join(parts) + ")"

    def _eq(self, other):
        if not isinstance(other, cls):
            return False
        for fn in fields:
            if getattr(self, fn) != getattr(other, fn):
                return False
        return True

    cls.__init__ = _init
    cls.__repr__ = _repr
    cls.__eq__   = _eq
    cls._fields  = tuple(fields)
    return cls


def _is_dunder(name):
    return name.startswith("__") and name.endswith("__")


def dataclass(cls=None, **kwargs):
    # Allow @dataclass and @dataclass(eq=True) forms.
    if cls is None:
        # Called with kwargs — return a decorator.
        def _wrap(c):
            return dataclass(c)
        return _wrap
    # Look for `_fields` class attribute giving the explicit order.
    fields = getattr(cls, "_fields", None)
    if fields is None:
        # Auto-detect: walk dir(cls) for non-method attributes.
        # First try __annotations__ if present.
        anns = getattr(cls, "__annotations__", None)
        if anns:
            fields = list(anns.keys()) if hasattr(anns, "keys") else list(anns)
        else:
            # Fall back: inspect class-level non-callable, non-dunder attrs.
            fields = []
            for name in dir(cls):
                if _is_dunder(name): continue
                if name.startswith("_"): continue
                try:
                    v = getattr(cls, name)
                except Exception:
                    continue
                if callable(v): continue
                fields.append(name)
    return _build_dataclass(cls, list(fields))


def make_dataclass(typename, fields):
    class _DC: pass
    _DC.__name__ = typename
    return _build_dataclass(_DC, list(fields))


def asdict(obj):
    fields = getattr(obj, "_fields", None)
    if fields is None:
        raise TypeError("asdict: not a dataclass")
    out = {}
    for fn in fields:
        out[fn] = getattr(obj, fn)
    return out


def astuple(obj):
    fields = getattr(obj, "_fields", None)
    if fields is None:
        raise TypeError("astuple: not a dataclass")
    return tuple(getattr(obj, fn) for fn in fields)


def fields(obj):
    fs = getattr(obj, "_fields", None)
    if fs is None:
        raise TypeError("fields: not a dataclass")
    return fs


__all__ = ["dataclass", "make_dataclass", "asdict", "astuple", "fields"]
