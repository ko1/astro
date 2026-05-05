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
    for m in members:
        setattr(cls, m.name, m)

    return cls


def _is_dunder(name):
    return name.startswith("__") and name.endswith("__")


class EnumMeta(type):
    def __new__(meta, name, bases, attrs):
        # Collect non-dunder, non-method attributes as enum members.
        items = []
        for key in list(attrs):
            if _is_dunder(key): continue
            v = attrs[key]
            if callable(v) and not isinstance(v, _Auto): continue
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
        cls = type.__call__(type, name, bases, attrs) if False else type(name, bases, attrs)
        cls._members_ = []
        cls._by_name_ = {}
        for n, v in items:
            m = _EnumMember(name, n, v)
            cls._members_.append(m)
            cls._by_name_[n] = m
            setattr(cls, n, m)
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


__all__ = ["Enum", "_make_enum", "auto"]
