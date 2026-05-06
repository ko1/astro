# pystro stdlib `dataclasses` (minimal).

class _Field:
    __slots__ = ("default", "default_factory", "init", "repr_", "compare", "metadata")
    def __init__(self, default=None, default_factory=None,
                 init=True, repr_=True, compare=True, metadata=None):
        self.default = default
        self.default_factory = default_factory
        self.init = init
        self.repr_ = repr_
        self.compare = compare
        self.metadata = metadata
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
                d = getattr(cls, fn)
                # _Field with default_factory: invoke per-instance.
                if isinstance(d, _Field):
                    if d.default_factory is not None:
                        setattr(self, fn, d.default_factory())
                    else:
                        setattr(self, fn, d.default)
                else:
                    setattr(self, fn, d)
            # else: leave unset
        # Call __post_init__ if defined.
        post = getattr(self, "__post_init__", None)
        if post is not None and post is not _init:
            post()

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
    # Walk MRO to collect inherited fields first; own annotations append/override.
    inherited = []
    try:
        mro = cls.__mro__
    except AttributeError:
        mro = [cls]
    for base in reversed(mro[1:]):  # walk from object up to direct parent
        bf = getattr(base, "_fields", None)
        if bf:
            for f in bf:
                if f not in inherited: inherited.append(f)
    # Look for `_fields` class attribute giving the explicit order.
    # Use OWN _fields only — inherited ones are already in `inherited`.
    own_dict = getattr(cls, "__dict__", {})
    fields = own_dict.get("_fields") if hasattr(own_dict, "get") else None
    if fields is None:
        # Auto-detect: walk dir(cls) for non-method attributes.
        # First try __annotations__ if present.
        anns = getattr(cls, "__annotations__", None)
        if anns:
            own = list(anns.keys()) if hasattr(anns, "keys") else list(anns)
            fields = list(inherited)
            for f in own:
                if f not in fields: fields.append(f)
        else:
            fields = list(inherited)
            # Fall back: inspect class-level non-callable, non-dunder attrs.
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


_MISSING = object()


def field(default=_MISSING, default_factory=_MISSING, init=True,
          repr=True, compare=True, metadata=None):
    return _Field(
        default=None if default is _MISSING else default,
        default_factory=None if default_factory is _MISSING else default_factory,
        init=init, repr_=repr, compare=compare, metadata=metadata,
    )


def fields(class_or_instance):
    fs = getattr(class_or_instance, "_fields", None)
    if fs is None:
        raise TypeError("fields() on non-dataclass")
    out = []
    for n in fs:
        out.append(_Field(default=getattr(class_or_instance, n, None)))
        out[-1].name = n  # add name attr for convenience
    return tuple(out)


def is_dataclass(obj):
    return hasattr(obj, "_fields")


def replace(obj, **changes):
    fs = getattr(obj, "_fields", None)
    if fs is None:
        raise TypeError("replace() on non-dataclass")
    new_args = {}
    for n in fs:
        new_args[n] = changes.get(n, getattr(obj, n))
    return type(obj)(**new_args)


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


__all__ = ["dataclass", "make_dataclass", "asdict", "astuple", "fields",
           "field", "is_dataclass", "replace"]
