"""pystro stub for `_collections` (CPython C accelerator for `collections`).

CPython's `Lib/collections/__init__.py` imports `deque`, `defaultdict`,
`_deque_iterator` from `_collections`.  Provide pure-Python versions
here so the import chain works (we can't `from collections import …`
because that would loop back here)."""


class deque:
    def __init__(self, iterable=(), maxlen=None):
        self._items = list(iterable)
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
    def pop(self):
        if not self._items: raise IndexError("pop from empty deque")
        return self._items.pop()
    def popleft(self):
        if not self._items: raise IndexError("pop from empty deque")
        return self._items.pop(0)
    def extend(self, it):
        for x in it: self.append(x)
    def extendleft(self, it):
        for x in it: self.appendleft(x)
    def clear(self):
        self._items.clear()
    def __len__(self): return len(self._items)
    def __iter__(self): return iter(self._items)
    def __reversed__(self): return reversed(self._items)
    def __contains__(self, x): return x in self._items
    def __getitem__(self, i): return self._items[i]
    def __setitem__(self, i, v): self._items[i] = v
    def __delitem__(self, i): del self._items[i]
    def __repr__(self):
        if self.maxlen is not None:
            return f"deque({self._items!r}, maxlen={self.maxlen})"
        return f"deque({self._items!r})"
    def __eq__(self, o):
        if isinstance(o, deque): return self._items == o._items
        return NotImplemented
    def __bool__(self): return bool(self._items)
    def copy(self): return deque(self._items, maxlen=self.maxlen)
    def count(self, x): return self._items.count(x)
    def remove(self, x): self._items.remove(x)
    def reverse(self): self._items.reverse()
    def rotate(self, n=1):
        if not self._items: return
        n = n % len(self._items)
        self._items = self._items[-n:] + self._items[:-n]
    def index(self, x, start=0, stop=None):
        if stop is None: stop = len(self._items)
        for i in range(start, stop):
            if self._items[i] == x: return i
        raise ValueError(f"{x!r} not in deque")


class defaultdict(dict):
    def __init__(self, default_factory=None, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.default_factory = default_factory
    def __missing__(self, key):
        if self.default_factory is None: raise KeyError(key)
        v = self.default_factory()
        self[key] = v
        return v
    def __getitem__(self, key):
        try: return super().__getitem__(key)
        except KeyError: return self.__missing__(key)
    def __repr__(self):
        return f"defaultdict({self.default_factory!r}, {dict.__repr__(self)})"
    def copy(self):
        return defaultdict(self.default_factory, self)


class OrderedDict(dict):
    """3.7+ regular dicts preserve insertion order, so OrderedDict is
    mostly a thin wrapper."""
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
    def move_to_end(self, key, last=True):
        v = self.pop(key)
        if last:
            self[key] = v
        else:
            new = OrderedDict({key: v})
            new.update(self)
            self.clear()
            self.update(new)
    def popitem(self, last=True):
        if not self: raise KeyError("dictionary is empty")
        if last:
            k = list(self.keys())[-1]
        else:
            k = next(iter(self))
        return (k, self.pop(k))
    def __reversed__(self):
        return reversed(list(self.keys()))


class _deque_iterator:
    def __init__(self, dq, idx=0):
        self.dq = dq
        self.idx = idx
    def __iter__(self): return self
    def __next__(self):
        if self.idx >= len(self.dq): raise StopIteration
        v = self.dq[self.idx]
        self.idx += 1
        return v


__all__ = ["deque", "defaultdict", "OrderedDict", "_deque_iterator"]
