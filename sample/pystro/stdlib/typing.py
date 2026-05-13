# pystro stdlib `typing` (minimal — type hints are runtime no-ops).
#
# Pystro discards function/variable annotations at parse time, so
# all of these are best-effort placeholder objects.  Subscripting
# (`List[int]`) returns the original generic, since pystro doesn't
# enforce type parameters.

class _GenericAlias:
    def __init__(self, origin, name, args=()):
        self._origin = origin
        self._name = name
        self.__args__ = args if isinstance(args, tuple) else (args,)
    @property
    def origin(self):
        return self._origin
    @property
    def __origin__(self):
        # CPython's typing._GenericAlias.__origin__ is the raw `list`,
        # `dict`, etc. — used by `get_origin()`.
        return self._origin
    def __getitem__(self, params):
        # `List[int]` returns a new _GenericAlias remembering the args.
        # CPython's typing.List[int] reprs as 'typing.List[int]'.
        if isinstance(params, tuple):
            new_args = self.__args__ + params
        else:
            new_args = self.__args__ + (params,)
        return _GenericAlias(self._origin, self._name, new_args)
    def __repr__(self):
        if not self.__args__:
            return self._name
        def _fmt(a):
            if a is type(None) or a is None:
                return "None"
            if a is Ellipsis:
                return "..."
            if isinstance(a, type):
                return getattr(a, "__qualname__", None) or a.__name__
            if isinstance(a, _GenericAlias):
                return repr(a)
            return repr(a)
        return self._name + "[" + ", ".join(_fmt(a) for a in self.__args__) + "]"
    def __call__(self, *args, **kw):
        return self._origin(*args, **kw)
    def __eq__(self, other):
        if not isinstance(other, _GenericAlias):
            return NotImplemented
        return self._origin is other._origin and self.__args__ == other.__args__
    def __hash__(self):
        return hash((id(self._origin), self.__args__))


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
T = "T"
K = "K"
V = "V"
S = "S"
KT = "KT"
VT = "VT"
T_co = "T_co"
T_contra = "T_contra"

# Common Generic-Alias bound to a concrete type — pystro treats these
# as opaque sentinels (you can subscript them via _GenericAlias).
IO = _GenericAlias(lambda x: x, "typing.IO")
TextIO = _GenericAlias(lambda x: x, "typing.TextIO")
Text = str
BinaryIO = _GenericAlias(lambda x: x, "typing.BinaryIO")
Pattern = _GenericAlias(lambda x: x, "typing.Pattern")
Match = _GenericAlias(lambda x: x, "typing.Match")
Awaitable = _GenericAlias(lambda x: x, "typing.Awaitable")
Coroutine = _GenericAlias(lambda x: x, "typing.Coroutine")
AsyncIterable = _GenericAlias(lambda x: x, "typing.AsyncIterable")
AsyncIterator = _GenericAlias(lambda x: x, "typing.AsyncIterator")
AsyncGenerator = _GenericAlias(lambda x: x, "typing.AsyncGenerator")
ContextManager = _GenericAlias(lambda x: x, "typing.ContextManager")
AsyncContextManager = _GenericAlias(lambda x: x, "typing.AsyncContextManager")
Mapping = _GenericAlias(dict, "typing.Mapping")
MutableMapping = _GenericAlias(dict, "typing.MutableMapping")
Sequence = _GenericAlias(list, "typing.Sequence")
MutableSequence = _GenericAlias(list, "typing.MutableSequence")
Container = _GenericAlias(lambda x: x, "typing.Container")
Collection = _GenericAlias(lambda x: x, "typing.Collection")
KeysView = _GenericAlias(lambda x: x, "typing.KeysView")
ValuesView = _GenericAlias(lambda x: x, "typing.ValuesView")
ItemsView = _GenericAlias(lambda x: x, "typing.ItemsView")
MappingView = _GenericAlias(lambda x: x, "typing.MappingView")
Reversible = _GenericAlias(lambda x: x, "typing.Reversible")
AbstractSet = _GenericAlias(set, "typing.AbstractSet")
MutableSet = _GenericAlias(set, "typing.MutableSet")
DefaultDict = _GenericAlias(dict, "typing.DefaultDict")
OrderedDict = _GenericAlias(dict, "typing.OrderedDict")
Counter = _GenericAlias(dict, "typing.Counter")
ChainMap = _GenericAlias(dict, "typing.ChainMap")
Deque = _GenericAlias(list, "typing.Deque")
ByteString = _GenericAlias(bytes, "typing.ByteString")
NamedTupleMeta = type("NamedTupleMeta", (type,), {})
TypedDict = _GenericAlias(dict, "typing.TypedDict")
TYPE_CHECKING = False


class ForwardRef:
    """typing.ForwardRef — placeholder for string-based type
    references.  Pystro doesn't evaluate annotations at runtime, so
    just store the source string."""
    __slots__ = ("__forward_arg__", "__forward_evaluated__", "__forward_value__")
    def __init__(self, arg, is_argument=True, module=None, is_class=False):
        self.__forward_arg__ = arg
        self.__forward_evaluated__ = False
        self.__forward_value__ = None
    def __eq__(self, other):
        return isinstance(other, ForwardRef) and self.__forward_arg__ == other.__forward_arg__
    def __hash__(self):
        return hash(self.__forward_arg__)
    def __repr__(self):
        return f"ForwardRef({self.__forward_arg__!r})"


class _SpecialForm:
    """typing.Final / typing.ClassVar / typing.Literal share this
    type at the Python level.  Provided for isinstance checks."""
    def __init__(self, name):
        self._name = name
    def __getitem__(self, key):
        return self
    def __repr__(self):
        return "typing." + self._name


def cast(typ, val):
    return val


def runtime(cls):
    return cls


# typing.ParamSpec / ParamSpecArgs / ParamSpecKwargs — PEP 612 generics.
class ParamSpecArgs:
    def __init__(self, origin=None): self.__origin__ = origin


class ParamSpecKwargs:
    def __init__(self, origin=None): self.__origin__ = origin


# Re-bind ParamSpec to have args/kwargs attributes.
class _ParamSpec:
    def __init__(self, name, *args, **kwargs):
        self.__name__ = name
        self.args = ParamSpecArgs(self)
        self.kwargs = ParamSpecKwargs(self)
    def __repr__(self): return "~" + self.__name__
    def __mro_entries__(self, bases): return ()


# Don't override the existing GenericAlias-based ParamSpec; provide a
# more capable callable form so `P = ParamSpec("P")` works.  Tests then
# read `P.args` / `P.kwargs` / `P.__name__`.
def ParamSpec(name, *args, **kwargs):  # noqa: F811
    return _ParamSpec(name, *args, **kwargs)


TypeGuard = _GenericAlias(lambda x: x, "typing.TypeGuard")


class TypeAliasType:
    """PEP 695 type alias (3.12+).  Pystro treats it as a transparent
    name carrier — value is stored as `__value__`."""
    def __init__(self, name, value, *, type_params=()):
        self.__name__ = name
        self.__value__ = value
        self.__type_params__ = type_params
    def __repr__(self):
        return self.__name__
    def __class_getitem__(cls, item):
        return cls
TypeIs = _GenericAlias(lambda x: x, "typing.TypeIs")
NotRequired = _GenericAlias(lambda x: x, "typing.NotRequired")
ClassVar = _GenericAlias(lambda x: x, "typing.ClassVar")
Final = _GenericAlias(lambda x: x, "typing.Final")
NewType = NewType  # already defined
NoReturn = _GenericAlias(lambda: None, "typing.NoReturn")


def is_protocol(cls):
    return False


def get_protocol_members(cls):
    return frozenset()


class _CallableGenericAlias(_GenericAlias):
    pass


# Sentinels CPython exposes via internal globals.
_AnnotatedAlias = _GenericAlias
_TypedDictMeta = type
Never = _GenericAlias(lambda: None, "typing.Never")
LiteralString = _GenericAlias(str, "typing.LiteralString")
Required = _GenericAlias(lambda x: x, "typing.Required")
NotRequired = _GenericAlias(lambda x: x, "typing.NotRequired")
Unpack = _GenericAlias(lambda x: x, "typing.Unpack")
TypeAlias = _GenericAlias(lambda x: x, "typing.TypeAlias")
ParamSpec = _GenericAlias(lambda x: x, "typing.ParamSpec")
TypeVarTuple = _GenericAlias(lambda x: x, "typing.TypeVarTuple")
Concatenate = _GenericAlias(lambda x: x, "typing.Concatenate")
NoDefault = object()


def assert_never(value):
    """CPython 3.11+: signal exhaustive match failure at the type-checker;
    at runtime, raise to mirror CPython semantics."""
    raise AssertionError(f"Expected code to be unreachable; got {value!r}")


def assert_type(value, type_):
    return value


def reveal_type(value):
    return value


def override(fn):
    return fn


def is_typeddict(tp):
    return False


def get_overloads(fn):
    return []


def clear_overloads():
    pass


def overload(fn):
    return fn


def final(fn):
    """@typing.final — no-op runtime decorator."""
    return fn


def dataclass_transform(*args, **kwargs):
    def deco(fn): return fn
    if args and callable(args[0]):
        return args[0]
    return deco


def no_type_check(fn):
    return fn


def no_type_check_decorator(fn):
    return fn


def get_type_hints(obj, globalns=None, localns=None, include_extras=False):
    return {}


def get_origin(tp):
    return None


def get_args(tp):
    return ()
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


def get_origin(tp):
    """Return the unsubscripted origin of a generic alias.  Pystro's
    PEP 585 returns the class itself for `list[int]`, so we return None
    in that case (CPython would return `list`).  For typing.List etc
    return the underlying type."""
    if isinstance(tp, _GenericAlias):
        return tp.origin
    return None


def get_args(tp):
    """Return the tuple of arguments to a generic alias.  Pystro doesn't
    track these — return ()."""
    if isinstance(tp, _GenericAlias):
        return ()
    return ()


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
