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


# `Enum` as a marker base — pystro can't intercept class-definition syntax,
# so users typically do `MyEnum = _make_enum("MyEnum", {...})`.
class Enum:
    pass


def auto():
    # Sentinel — when a tool walks user-defined enum members and sees
    # this object, it auto-assigns sequential ints.  In v0 we can't
    # intercept class def, so this is informational only.
    return _Auto()

class _Auto:
    pass


__all__ = ["Enum", "_make_enum", "auto"]
