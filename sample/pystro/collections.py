# pystro stdlib `collections` (minimal).
#
# pystro's dict is already insertion-ordered, so OrderedDict is a thin
# alias.  defaultdict / Counter / namedtuple are present but limited:
# they don't subclass dict (pystro's dict isn't a real class yet).

# OrderedDict — alias to dict (pystro dict preserves insertion order).
def OrderedDict(*args):
    d = {}
    if len(args) == 1:
        for k, v in args[0]:
            d[k] = v
    return d


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
    def __init__(self, iterable=None):
        self._d = {}
        if iterable is None:
            return
        for x in iterable:
            self._d[x] = self._d.get(x, 0) + 1
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
    def __repr__(self):
        return "Counter(" + repr(self._d) + ")"


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
def namedtuple(typename, fields):
    if isinstance(fields, str):
        fields = fields.replace(",", " ").split()
    fields = list(fields)

    class _NT:
        _fields = tuple(fields)
        def __init__(self, *args, **kwargs):
            if args and kwargs:
                raise TypeError("mix args and kwargs not supported")
            if kwargs:
                self._values = tuple(kwargs[f] for f in self._fields)
            else:
                if len(args) != len(self._fields):
                    raise TypeError("namedtuple arity mismatch")
                self._values = args
            for i, name in enumerate(self._fields):
                setattr(self, name, args[i] if args else kwargs[name])
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

    _NT.__name__ = typename
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
    def new_child(self, m=None):
        return ChainMap(m if m is not None else {}, *self.maps)
    @property
    def parents(self):
        return ChainMap(*self.maps[1:])


def UserDict(initial=None):
    return dict(initial) if initial is not None else {}


def UserList(initial=None):
    return list(initial) if initial is not None else []


def UserString(s):
    return s


__all__ = ["OrderedDict", "defaultdict", "Counter", "deque",
           "namedtuple", "ChainMap", "UserDict", "UserList", "UserString"]
