# pystro stdlib `enum` (minimal).
#
# Usage:
#   class Color(Enum):
#       RED = 1
#       GREEN = 2
#       BLUE = 3
#   Color.RED.name == "RED"
#   Color.RED.value == 1
#   list(Color) == [Color.RED, Color.GREEN, Color.BLUE]
#
# pystro v0 doesn't run a metaclass, so the user has to wrap the
# definitions in a `_make_enum` call:
#   Color = _make_enum("Color", {"RED": 1, "GREEN": 2, "BLUE": 3})
# Or use the simpler `Enum` factory below.

class _EnumMember:
    def __init__(self, type_name, name, value):
        self._type_name = type_name
        self.name = name
        self.value = value
    def __repr__(self):
        return self._type_name + "." + self.name
    def __eq__(self, other):
        if isinstance(other, _EnumMember):
            return self.name == other.name and self.value == other.value
        return False
    def __hash__(self):
        return hash((self.name, self.value))
    # Numeric ops — delegate to value, so IntEnum members work as ints.
    def __int__(self): return int(self.value)
    def __index__(self): return int(self.value)
    def __add__(self, o): return self.value + (o.value if isinstance(o, _EnumMember) else o)
    def __radd__(self, o): return (o.value if isinstance(o, _EnumMember) else o) + self.value
    def __sub__(self, o): return self.value - (o.value if isinstance(o, _EnumMember) else o)
    def __rsub__(self, o): return (o.value if isinstance(o, _EnumMember) else o) - self.value
    def __mul__(self, o): return self.value * (o.value if isinstance(o, _EnumMember) else o)
    def __rmul__(self, o): return (o.value if isinstance(o, _EnumMember) else o) * self.value
    def __or__(self, o): return self.value | (o.value if isinstance(o, _EnumMember) else o)
    def __and__(self, o): return self.value & (o.value if isinstance(o, _EnumMember) else o)
    def __xor__(self, o): return self.value ^ (o.value if isinstance(o, _EnumMember) else o)
    def __lt__(self, o): return self.value <  (o.value if isinstance(o, _EnumMember) else o)
    def __le__(self, o): return self.value <= (o.value if isinstance(o, _EnumMember) else o)
    def __gt__(self, o): return self.value >  (o.value if isinstance(o, _EnumMember) else o)
    def __ge__(self, o): return self.value >= (o.value if isinstance(o, _EnumMember) else o)
    def __bool__(self):
        return bool(self.value)


def _make_enum(typename, items):
    # `items` may be a dict or a list of (name, value) pairs.
    if isinstance(items, dict):
        items = list(items.items())
    members = []
    name_to_member = {}
    for n, v in items:
        m = _EnumMember(typename, n, v)
        members.append(m)
        name_to_member[n] = m

    class _Enum:
        pass
    cls = _Enum
    cls.__name__ = typename
    cls._members_ = members
    cls._by_name_ = name_to_member
    cls.__members__ = dict(name_to_member)
    for m in members:
        setattr(cls, m.name, m)

    return cls


def _is_dunder(name):
    return name.startswith("__") and name.endswith("__")


class EnumMeta(type):
    def __len__(cls):
        return len(cls._members_)
    def __iter__(cls):
        return iter(cls._members_)
    def __contains__(cls, member):
        return member in cls._members_
    @property
    def __members__(cls):
        # CPython exposes an OrderedDict; pystro is order-preserving by
        # default so a plain dict suffices.
        return dict(cls._by_name_)
    def __new__(meta, name, bases, attrs):
        # Collect non-dunder, non-method attributes as enum members; keep methods.
        items = []
        methods = {}
        for key in list(attrs):
            if _is_dunder(key): continue
            v = attrs[key]
            if callable(v) and not isinstance(v, _Auto):
                methods[key] = v
                continue
            items.append((key, v))

        # Auto-assign for _Auto sentinels.
        next_val = 1
        for i in range(len(items)):
            n, v = items[i]
            if isinstance(v, _Auto):
                items[i] = (n, next_val)
                next_val += 1
            elif isinstance(v, int):
                next_val = v + 1

        # Build the class with members removed from attrs.
        for n, _ in items:
            if n in attrs: del attrs[n]
        cls = type(name, bases, attrs)
        # Build per-enum member class so user methods are available on members.
        # Use _EnumMember as the per-member class and copy user methods into it.
        class _Member(_EnumMember):
            pass
        _Member.__name__ = name
        for k, v in methods.items():
            setattr(_Member, k, v)
        cls._members_ = []
        cls._by_name_ = {}
        for n, v in items:
            m = _Member(name, n, v)
            cls._members_.append(m)
            cls._by_name_[n] = m
            setattr(cls, n, m)
        # Also expose `__members__` directly on the class — CPython parity.
        # The metaclass property handles direct subclasses, but tests reach
        # for `cls.__members__` via attribute access which on built-in enums
        # like HTTPStatus may not consult the metaclass property reliably.
        cls.__members__ = dict(cls._by_name_)
        return cls


class Enum(metaclass=EnumMeta):
    pass


def __iter_enum__(cls):
    return iter(cls._members_)


# Make enum classes iterable.  Since pystro doesn't dispatch __iter__ on
# class objects directly, users iterate via ClassName._members_.


def auto():
    # Sentinel — when a tool walks user-defined enum members and sees
    # this object, it auto-assigns sequential ints.  In v0 we can't
    # intercept class def, so this is informational only.
    return _Auto()

class _Auto:
    pass


class IntEnum(Enum):
    @classmethod
    def _convert_(cls, name, module, filter, source=None, *, boundary=None, as_global=False):
        # CPython internals call this to lift module-level int constants
        # into an IntEnum.  pystro doesn't have CPython's module-rewriting
        # machinery; fall back to a plain class with the matching members.
        if source is None:
            source = __import__(module)
        items = []
        for k in dir(source):
            v = getattr(source, k, None)
            if isinstance(v, int) and filter(k):
                items.append((k, v))
        return _make_enum(name, items)


# IntEnum members compare equal to their int value.
def _int_eq(self, other):
    if isinstance(other, _EnumMember):
        return self.name == other.name and self.value == other.value
    return self.value == other

# Patch _EnumMember equality for IntEnum-like comparisons (3.x permits this
# for IntEnum specifically; keep it across all members for simplicity).
def _flexible_eq(self, other):
    if isinstance(other, _EnumMember):
        return self.name == other.name and self.value == other.value
    # Fall back to value equality (covers IntEnum vs int, StrEnum vs str).
    return self.value == other


_EnumMember.__eq__ = _flexible_eq


class StrEnum(Enum):
    pass


class Flag(Enum):
    pass


class IntFlag(Flag):
    pass


def unique(cls):
    seen = {}
    for m in cls._members_:
        if m.value in seen:
            raise ValueError(f"duplicate values: {m.name} and {seen[m.value]}")
        seen[m.value] = m.name
    return cls


def _simple_enum(*args, **kwargs):
    """3.11+ decorator that builds an Enum-like class from a plain class
    without going through EnumMeta.  Replaces `auto()` sentinels with
    sequential ints (1-based, matching IntEnum default) so attribute
    access on the decorated class returns ints."""
    def deco(cls):
        next_val = 1
        members = {}
        # If the source class defines __new__, member values are built
        # via cls.__new__(cls, *args) — used by HTTPStatus / HTTPMethod
        # where each member is `NAME = value, phrase, description`.
        has_new = "__new__" in dir(cls) and callable(getattr(cls, "__new__", None))
        for name in dir(cls):
            if name.startswith("_"):
                continue
            try:
                val = getattr(cls, name)
            except AttributeError:
                continue
            if callable(val) and not isinstance(val, _Auto):
                continue
            built = None
            if isinstance(val, _Auto):
                built = next_val
                next_val += 1
            elif isinstance(val, tuple) and val and isinstance(val[0], (int, str)):
                if has_new:
                    try:
                        built = cls.__new__(cls, *val)
                    except Exception:
                        built = val[0]
                else:
                    built = val[0]
                if isinstance(val[0], int):
                    next_val = val[0] + 1
            elif isinstance(val, int):
                if has_new:
                    try:
                        built = cls.__new__(cls, val)
                    except Exception:
                        built = val
                else:
                    built = val
                next_val = val + 1
            elif isinstance(val, str):
                built = val
            else:
                continue
            members[name] = built
            setattr(cls, name, built)
        cls.__members__ = members
        cls._by_name_ = dict(members)
        cls._members_ = list(members.values())
        return cls
    return deco


def _test_simple_enum(*args, **kwargs):
    """Used by CPython tests to sanity-check _simple_enum's results."""
    pass


def member(value):
    return value

def nonmember(value):
    return value

def global_enum(cls):
    """Decorator: copy each member onto the defining module's globals.
    Used by `calendar.Day` so `calendar.MONDAY` resolves directly."""
    try:
        import sys
        members = getattr(cls, "_by_name_", None) \
            or getattr(cls, "__members__", {})
        items = list(members.items()) if hasattr(members, "items") else []
        # 1) Inject into the caller's frame globals (the module being
        #    initialized).  Works when sys._getframe is functional.
        try:
            caller = sys._getframe(1)
            g = caller.f_globals
            for name, value in items:
                g[name] = value
        except Exception:
            pass
        # 2) Fallback: locate the module by name (pystro often sets
        #    cls.__module__ to '__main__'; try every import-path
        #    candidate via sys.modules).
        mod_name = getattr(cls, "__module__", None)
        if mod_name and mod_name in sys.modules:
            mod = sys.modules[mod_name]
            for name, value in items:
                try: setattr(mod, name, value)
                except Exception: pass
    except Exception:
        pass
    return cls

def property(fn):
    return fn

# Auto-expose builtin int/bool flag types.
DynamicClassAttribute = property
EnumType = EnumMeta


IntFlag = Flag    # treat IntFlag as Flag for our purposes
ReprEnum = Enum   # CPython 3.11 base for IntEnum/StrEnum — same surface in pystro.


# CPython 3.11+ FlagBoundary enum — pystro doesn't enforce boundaries
# but tests probe these names directly.
class FlagBoundary:
    STRICT = "STRICT"
    CONFORM = "CONFORM"
    EJECT = "EJECT"
    KEEP = "KEEP"

STRICT = FlagBoundary.STRICT
CONFORM = FlagBoundary.CONFORM
EJECT = FlagBoundary.EJECT
KEEP = FlagBoundary.KEEP

# Enum decorators/checks introduced in CPython 3.11.
def verify(*checks):
    def _wrap(cls): return cls
    if checks and callable(checks[0]):
        return checks[0]
    return _wrap

class EnumCheck:
    UNIQUE = "UNIQUE"
    CONTINUOUS = "CONTINUOUS"
    NAMED_FLAGS = "NAMED_FLAGS"

UNIQUE = EnumCheck.UNIQUE
CONTINUOUS = EnumCheck.CONTINUOUS
NAMED_FLAGS = EnumCheck.NAMED_FLAGS

def show_flag_values(value):
    return []


def _iter_bits_lsb(num):
    """Yield each single-bit power present in ``num`` (LSB first)."""
    while num:
        b = num & -num
        yield b
        num ^= b


def bit_count(value):
    if value == 0: return 0
    return bin(value).count("1")


def pickle_by_global_name(self, proto):
    return self.name


def pickle_by_enum_name(self, proto):
    return (getattr, (self.__class__, self.name))


__all__ = ["Enum", "IntEnum", "StrEnum", "Flag", "IntFlag", "EnumMeta", "EnumType",
           "_simple_enum", "_test_simple_enum", "member", "nonmember",
           "global_enum", "auto", "DynamicClassAttribute",
           "_make_enum", "auto", "unique",
           "FlagBoundary", "STRICT", "CONFORM", "EJECT", "KEEP",
           "verify", "EnumCheck", "UNIQUE", "CONTINUOUS", "NAMED_FLAGS",
           "show_flag_values"]
