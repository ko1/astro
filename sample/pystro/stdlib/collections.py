# pystro stdlib `collections` (minimal).
#
# pystro's dict is already insertion-ordered, so OrderedDict is a thin
# alias.  defaultdict / Counter / namedtuple are present but limited:
# they don't subclass dict (pystro's dict isn't a real class yet).

# OrderedDict — pystro's dict already insertion-ordered.  Subclass adds
# the move_to_end and popitem(last=) helpers.
class OrderedDict(dict):
    def __init__(self, items=None):
        super().__init__()
        if items is not None:
            if hasattr(items, "items"):
                for k, v in items.items(): self[k] = v
            else:
                for k, v in items: self[k] = v
    def move_to_end(self, key, last=True):
        v = self[key]
        del self[key]
        if last:
            self[key] = v
        else:
            # Move to start: build a fresh dict, prepend, then refill.
            saved = list(self.items())
            super().clear()
            self[key] = v
            for k, val in saved:
                self[k] = val
    def popitem(self, last=True):
        if not self: raise KeyError("dict empty")
        keys = list(self.keys())
        k = keys[-1] if last else keys[0]
        v = self[k]
        del self[k]
        return (k, v)
    def __eq__(self, other):
        # OrderedDict == OrderedDict: compare values *and* order.
        # OrderedDict == dict: order-insensitive (CPython behaviour).
        if isinstance(other, OrderedDict):
            return list(self.items()) == list(other.items())
        if isinstance(other, dict):
            d = {}
            for k, v in self.items(): d[k] = v
            return d == other
        return NotImplemented
    def __ne__(self, other):
        r = self.__eq__(other)
        if r is NotImplemented: return r
        return not r
    def __repr__(self):
        if not self:
            return "OrderedDict()"
        items = ", ".join(repr(k) + ": " + repr(v) for k, v in self.items())
        return "OrderedDict({" + items + "})"


class defaultdict:
    def __init__(self, default_factory=None):
        self._d = {}
        self.default_factory = default_factory
    def __getitem__(self, k):
        if k in self._d:
            return self._d[k]
        if self.default_factory is None:
            raise KeyError(k)
        v = self.default_factory()
        self._d[k] = v
        return v
    def __setitem__(self, k, v):
        self._d[k] = v
    def __contains__(self, k):
        return k in self._d
    def __iter__(self):
        for k in self._d:
            yield k
    def __len__(self):
        return len(self._d)
    def keys(self):    return self._d.keys()
    def values(self):  return self._d.values()
    def items(self):   return self._d.items()
    def get(self, k, default=None):
        return self._d.get(k, default)
    def __repr__(self):
        return "defaultdict(" + repr(self.default_factory) + ", " + repr(self._d) + ")"


class Counter:
    def __init__(self, iterable=None, **kwargs):
        self._d = {}
        if iterable is not None:
            if hasattr(iterable, "items") and not hasattr(iterable, "_fields"):
                # dict-like: copy counts directly
                for k, v in iterable.items():
                    self._d[k] = self._d.get(k, 0) + v
            else:
                for x in iterable:
                    self._d[x] = self._d.get(x, 0) + 1
        for k, v in kwargs.items():
            self._d[k] = self._d.get(k, 0) + v
    def __getitem__(self, k):
        return self._d.get(k, 0)
    def __setitem__(self, k, v):
        self._d[k] = v
    def __contains__(self, k):
        return k in self._d
    def __iter__(self):
        for k in self._d:
            yield k
    def __len__(self):
        return len(self._d)
    def keys(self):    return self._d.keys()
    def values(self):  return self._d.values()
    def items(self):   return self._d.items()
    def most_common(self, n=None):
        items = [(k, v) for k, v in self._d.items()]
        items.sort(key=lambda kv: -kv[1])
        if n is None:
            return items
        return items[:n]
    def subtract(self, iterable):
        if hasattr(iterable, "items"):
            for k, v in iterable.items():
                self._d[k] = self._d.get(k, 0) - v
        else:
            for x in iterable:
                self._d[x] = self._d.get(x, 0) - 1
    def update(self, iterable):
        if hasattr(iterable, "items"):
            for k, v in iterable.items():
                self._d[k] = self._d.get(k, 0) + v
        else:
            for x in iterable:
                self._d[x] = self._d.get(x, 0) + 1
    def total(self):
        return sum(self._d.values())
    def elements(self):
        for k, v in self._d.items():
            for _ in range(v):
                yield k
    def __add__(self, other):
        # Counter + Counter — sum counts; drop non-positive results.
        r = Counter()
        for k in self._d:
            v = self._d[k] + (other._d[k] if k in other._d else 0)
            if v > 0: r._d[k] = v
        for k in other._d:
            if k not in self._d and other._d[k] > 0:
                r._d[k] = other._d[k]
        return r
    def __sub__(self, other):
        # Multiset subtraction; keep only positives.
        r = Counter()
        for k in self._d:
            v = self._d[k] - (other._d[k] if k in other._d else 0)
            if v > 0: r._d[k] = v
        return r
    def __and__(self, other):
        # Multiset intersection: min of counts.
        r = Counter()
        for k in self._d:
            if k in other._d:
                v = min(self._d[k], other._d[k])
                if v > 0: r._d[k] = v
        return r
    def __or__(self, other):
        # Multiset union: max of counts.
        r = Counter()
        for k in self._d:
            v = max(self._d[k], other._d[k] if k in other._d else 0)
            if v > 0: r._d[k] = v
        for k in other._d:
            if k not in self._d and other._d[k] > 0:
                r._d[k] = other._d[k]
        return r
    def __pos__(self):
        return Counter({k: v for k, v in self._d.items() if v > 0})
    def __neg__(self):
        return Counter({k: -v for k, v in self._d.items() if v < 0})
    def __eq__(self, other):
        if isinstance(other, Counter):
            return self._d == other._d
        return False
    def __repr__(self):
        # CPython sorts by descending count for the repr — most_common
        # ordering — so equal Counters always look the same.
        items = sorted(self._d.items(), key=lambda kv: -kv[1])
        body = ", ".join(repr(k) + ": " + repr(v) for k, v in items)
        return "Counter({" + body + "})"


class deque:
    def __init__(self, iterable=None, maxlen=None):
        self._items = list(iterable) if iterable is not None else []
        self.maxlen = maxlen
        if maxlen is not None and len(self._items) > maxlen:
            self._items = self._items[-maxlen:]
    def append(self, x):
        self._items.append(x)
        if self.maxlen is not None and len(self._items) > self.maxlen:
            self._items.pop(0)
    def appendleft(self, x):
        self._items.insert(0, x)
        if self.maxlen is not None and len(self._items) > self.maxlen:
            self._items.pop()
    def extend(self, xs):
        for x in xs: self.append(x)
    def extendleft(self, xs):
        for x in xs: self.appendleft(x)
    def pop(self):
        return self._items.pop()
    def popleft(self):
        return self._items.pop(0)
    def rotate(self, n=1):
        if not self._items: return
        n = n % len(self._items)
        if n != 0:
            self._items = self._items[-n:] + self._items[:-n]
    def clear(self):
        self._items.clear()
    def count(self, x):
        return self._items.count(x)
    def index(self, x):
        return self._items.index(x)
    def remove(self, x):
        self._items.remove(x)
    def reverse(self):
        self._items.reverse()
    def __len__(self):
        return len(self._items)
    def __iter__(self):
        for x in self._items:
            yield x
    def __getitem__(self, i):
        return self._items[i]
    def __repr__(self):
        return "deque(" + repr(self._items) + ")"


# namedtuple — returns a class that exposes fields both by index and
# by attribute via __getattr__.
def namedtuple(typename, fields, *, defaults=None):
    if isinstance(fields, str):
        fields = fields.replace(",", " ").split()
    fields = list(fields)
    # defaults align right-most; e.g. fields=[a,b,c], defaults=[10,20]
    # → c=20 default, b=10 default, a required.
    field_defaults = {}
    if defaults:
        defaults = list(defaults)
        for i, d in enumerate(defaults):
            field_defaults[fields[len(fields) - len(defaults) + i]] = d

    class _NT:
        _fields = tuple(fields)
        _field_defaults = dict(field_defaults)
        def __init__(self, *args, **kwargs):
            if args and kwargs:
                # mix is OK in CPython namedtuple; fill remaining via kwargs
                pass
            values = []
            for i, f in enumerate(self._fields):
                if i < len(args):
                    values.append(args[i])
                elif f in kwargs:
                    values.append(kwargs[f])
                elif f in self._field_defaults:
                    values.append(self._field_defaults[f])
                else:
                    raise TypeError(f"missing arg {f}")
            self._values = tuple(values)
        def __getitem__(self, i):
            if isinstance(i, str):
                idx = self._fields.index(i)
                return self._values[idx]
            return self._values[i]
        def __len__(self):
            return len(self._values)
        def __iter__(self):
            for v in self._values:
                yield v
        def __eq__(self, o):
            if isinstance(o, _NT):
                return self._values == o._values
            if isinstance(o, tuple):
                return self._values == o
            return False
        def __hash__(self):
            return hash(self._values)
        def __repr__(self):
            parts = []
            for i in range(len(self._fields)):
                parts.append(self._fields[i] + "=" + repr(self._values[i]))
            return typename + "(" + ", ".join(parts) + ")"
        def _asdict(self):
            d = {}
            for i, name in enumerate(self._fields):
                d[name] = self._values[i]
            return d
        def _replace(self, **kwargs):
            new_values = list(self._values)
            for k, v in kwargs.items():
                idx = self._fields.index(k)
                new_values[idx] = v
            return _NT(*new_values)
        def count(self, x):
            return sum(1 for v in self._values if v == x)
        def index(self, x):
            return list(self._values).index(x)
        @classmethod
        def _make(cls, iterable):
            return cls(*iterable)

    _NT.__name__ = typename
    # Expose each field as a property so `Cls.field.__doc__ = ...` works
    # the way CPython's collections.namedtuple does.
    class _NTField:
        def __init__(self, idx):
            self._idx = idx
            self.__doc__ = ""
        def __get__(self, obj, owner=None):
            if obj is None:
                return self
            return obj._values[self._idx]
    for _i, _f in enumerate(_NT._fields):
        setattr(_NT, _f, _NTField(_i))
    return _NT


class ChainMap:
    def __init__(self, *maps):
        self.maps = list(maps) if maps else [{}]
    def __getitem__(self, key):
        for m in self.maps:
            if key in m:
                return m[key]
        raise KeyError(key)
    def get(self, key, default=None):
        for m in self.maps:
            if key in m:
                return m[key]
        return default
    def __contains__(self, key):
        for m in self.maps:
            if key in m:
                return True
        return False
    def __iter__(self):
        seen = set()
        for m in self.maps:
            for k in m:
                if k not in seen:
                    seen.add(k)
                    yield k
    def __len__(self):
        return len(list(iter(self)))
    def keys(self):
        return list(iter(self))
    def values(self):
        return [self[k] for k in self]
    def items(self):
        return [(k, self[k]) for k in self]
    def __setitem__(self, key, value):
        self.maps[0][key] = value
    def __delitem__(self, key):
        if key not in self.maps[0]:
            raise KeyError(key)
        del self.maps[0][key]
    def pop(self, key, *default):
        if key in self.maps[0]:
            return self.maps[0].pop(key)
        if default: return default[0]
        raise KeyError(key)
    def clear(self):
        self.maps[0].clear()
    def new_child(self, m=None):
        return ChainMap(m if m is not None else {}, *self.maps)
    @property
    def parents(self):
        return ChainMap(*self.maps[1:])


class UserDict:
    def __init__(self, initial=None):
        self.data = dict(initial) if initial is not None else {}
    def __getitem__(self, k): return self.data[k]
    def __setitem__(self, k, v): self.data[k] = v
    def __delitem__(self, k): del self.data[k]
    def __iter__(self): return iter(self.data)
    def __contains__(self, k): return k in self.data
    def __len__(self): return len(self.data)
    # CPython: UserDict.__repr__ returns repr(self.data), not
    # ClassName(...) — tests like mapping_tests.test_repr expect the dict
    # literal format.
    def __repr__(self): return repr(self.data)
    def keys(self): return self.data.keys()
    def values(self): return self.data.values()
    def items(self): return self.data.items()
    def get(self, k, default=None): return self.data.get(k, default)
    def update(self, other=None, **kw):
        if other is not None:
            if hasattr(other, "items"):
                for k, v in other.items(): self[k] = v
            else:
                for k, v in other: self[k] = v
        for k, v in kw.items(): self[k] = v
    def setdefault(self, k, d=None):
        if k not in self: self[k] = d
        return self[k]
    def pop(self, k, *args):
        if k in self.data: v = self.data.pop(k); return v
        if args: return args[0]
        raise KeyError(k)
    def clear(self): self.data.clear()
    def copy(self):
        c = type(self)()
        c.data = dict(self.data)
        return c
    @classmethod
    def fromkeys(cls, iterable, value=None):
        d = cls()
        for k in iterable:
            d[k] = value
        return d


class UserList:
    def __init__(self, initlist=None):
        self.data = []
        if initlist is not None:
            if type(initlist) is list:
                self.data[:] = initlist
            elif isinstance(initlist, UserList):
                self.data[:] = initlist.data[:]
            else:
                self.data = list(initlist)
    # CPython: UserList.__repr__ returns repr(self.data) — bare list form.
    def __repr__(self): return repr(self.data)
    def __lt__(self, other): return self.data <  (other.data if isinstance(other, UserList) else other)
    def __le__(self, other): return self.data <= (other.data if isinstance(other, UserList) else other)
    def __eq__(self, other): return self.data == (other.data if isinstance(other, UserList) else other)
    def __gt__(self, other): return self.data >  (other.data if isinstance(other, UserList) else other)
    def __ge__(self, other): return self.data >= (other.data if isinstance(other, UserList) else other)
    def __contains__(self, item): return item in self.data
    def __len__(self): return len(self.data)
    def __getitem__(self, i):
        if isinstance(i, slice): return self.__class__(self.data[i])
        return self.data[i]
    def __setitem__(self, i, item): self.data[i] = item
    def __delitem__(self, i): del self.data[i]
    def __iter__(self): return iter(self.data)
    def __add__(self, other):
        if isinstance(other, UserList):
            return self.__class__(self.data + other.data)
        elif isinstance(other, list):
            return self.__class__(self.data + other)
        return self.__class__(self.data + list(other))
    def __radd__(self, other):
        if isinstance(other, UserList): return self.__class__(other.data + self.data)
        elif isinstance(other, list): return self.__class__(other + self.data)
        return self.__class__(list(other) + self.data)
    def __iadd__(self, other):
        if isinstance(other, UserList):
            self.data += other.data
        elif isinstance(other, list):
            self.data += other
        else:
            self.data += list(other)
        return self
    def __mul__(self, n): return self.__class__(self.data * n)
    def __rmul__(self, n): return self.__class__(self.data * n)
    def __imul__(self, n): self.data *= n; return self
    def __copy__(self):
        inst = type(self).__new__(type(self))
        inst.data = self.data[:]
        return inst
    def append(self, item): self.data.append(item)
    def insert(self, i, item): self.data.insert(i, item)
    def pop(self, i=-1): return self.data.pop(i)
    def remove(self, item): self.data.remove(item)
    def clear(self): self.data.clear()
    def copy(self): return self.__class__(self)
    def count(self, item): return self.data.count(item)
    def index(self, item, *args): return self.data.index(item, *args)
    def reverse(self): self.data.reverse()
    def sort(self, *args, **kwds): self.data.sort(*args, **kwds)
    def extend(self, other):
        if isinstance(other, UserList): self.data.extend(other.data)
        else: self.data.extend(other)


class UserString:
    def __init__(self, s):
        self.data = str(s)
    def __str__(self): return self.data
    def __repr__(self): return type(self).__name__ + "(" + repr(self.data) + ")"
    def __getitem__(self, i): return self.data[i]
    def __len__(self): return len(self.data)
    def __iter__(self): return iter(self.data)
    def __contains__(self, x): return x in self.data
    def __eq__(self, o): return self.data == (o.data if isinstance(o, UserString) else o)
    def __add__(self, o): return type(self)(self.data + (o.data if isinstance(o, UserString) else o))
    def __mul__(self, n): return type(self)(self.data * n)
    def lower(self): return type(self)(self.data.lower())
    def upper(self): return type(self)(self.data.upper())


# collections.abc — minimal classes that classify common types via
# isinstance.  Pystro doesn't yet support metaclass __instancecheck__
# fully on user classes, so the actual checks are done by isinstance
# fallback (each class wraps a concrete builtin type).

class _AbcBase:
    pass

# Make these concrete classes that isinstance can check with a custom
# __class__ check.  We use simple "marker" classes; isinstance will
# return True for any instance via metaclass override.

class _AbcMeta(type):
    # Allow `Mapping[str, str]` subscript form by returning the class
    # itself (PEP 585-style).
    def __getitem__(cls, params):
        return cls
    def __instancecheck__(cls, obj):
        nm = cls.__name__
        if nm == "Iterable":
            return hasattr(obj, "__iter__") or isinstance(obj, (list, tuple, dict, set, frozenset, str, bytes, bytearray, range))
        if nm == "Iterator":
            return hasattr(obj, "__next__")
        if nm == "Sized":
            return hasattr(obj, "__len__") or isinstance(obj, (list, tuple, dict, set, frozenset, str, bytes, bytearray))
        if nm == "Container":
            return hasattr(obj, "__contains__") or isinstance(obj, (list, tuple, dict, set, frozenset, str, bytes, bytearray, range))
        if nm == "Hashable":
            try:
                hash(obj); return True
            except TypeError:
                return False
        if nm == "Callable":
            return callable(obj)
        if nm == "Mapping" or nm == "MutableMapping":
            return isinstance(obj, dict)
        if nm == "Sequence":
            return isinstance(obj, (list, tuple, str, bytes, bytearray, range))
        if nm == "MutableSequence":
            return isinstance(obj, (list, bytearray))
        if nm == "Set" or nm == "MutableSet":
            return isinstance(obj, (set, frozenset))
        if nm == "ByteString":
            return isinstance(obj, (bytes, bytearray))
        if nm == "Generator":
            return hasattr(obj, "send") and hasattr(obj, "throw")
        return type.__instancecheck__(cls, obj)


class _Abc:
    class Iterable(metaclass=_AbcMeta): pass
    class Iterator(metaclass=_AbcMeta): pass
    class Sized(metaclass=_AbcMeta): pass
    class Container(metaclass=_AbcMeta): pass
    class Hashable(metaclass=_AbcMeta): pass
    class Callable(metaclass=_AbcMeta): pass
    class Mapping(metaclass=_AbcMeta): pass
    class MutableMapping(metaclass=_AbcMeta): pass
    class Sequence(metaclass=_AbcMeta): pass
    class MutableSequence(metaclass=_AbcMeta): pass
    class Set(metaclass=_AbcMeta): pass
    class MutableSet(metaclass=_AbcMeta): pass
    class ByteString(metaclass=_AbcMeta): pass
    class Generator(metaclass=_AbcMeta): pass
abc = _Abc

__all__ = ["OrderedDict", "defaultdict", "Counter", "deque",
           "namedtuple", "ChainMap", "UserDict", "UserList", "UserString",
           "abc"]
