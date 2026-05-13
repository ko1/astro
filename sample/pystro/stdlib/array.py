"""pystro stub for `array` (CPython C extension).  Implements a minimal
list-backed array with Python-side type checking — sufficient for code
that just stores numeric values."""

_TYPECODES = {
    "b": (-128, 127, int),
    "B": (0, 255, int),
    "h": (-32768, 32767, int),
    "H": (0, 65535, int),
    "i": (-2**31, 2**31-1, int),
    "I": (0, 2**32-1, int),
    "l": (-2**63, 2**63-1, int),
    "L": (0, 2**64-1, int),
    "q": (-2**63, 2**63-1, int),
    "Q": (0, 2**64-1, int),
    "f": (None, None, float),
    "d": (None, None, float),
    "u": (None, None, str),
}


class array:
    typecodes = "bBhHiIlLqQfdu"

    def __init__(self, typecode, initializer=None):
        if typecode not in _TYPECODES:
            raise ValueError("bad typecode: " + typecode)
        self.typecode = typecode
        self._lo, self._hi, self._t = _TYPECODES[typecode]
        self._items = []
        if initializer is not None:
            self.extend(initializer)

    @property
    def itemsize(self):
        return {"b": 1, "B": 1, "h": 2, "H": 2,
                "i": 4, "I": 4, "l": 8, "L": 8,
                "q": 8, "Q": 8, "f": 4, "d": 8, "u": 4}[self.typecode]

    def append(self, x):
        if not isinstance(x, self._t):
            x = self._t(x)
        if self._lo is not None and (x < self._lo or x > self._hi):
            raise OverflowError("value out of range")
        self._items.append(x)

    def extend(self, seq):
        for x in seq: self.append(x)

    def __len__(self): return len(self._items)
    def __getitem__(self, i): return self._items[i]
    def __setitem__(self, i, v):
        if not isinstance(v, self._t): v = self._t(v)
        self._items[i] = v
    def __delitem__(self, i): del self._items[i]
    def __iter__(self): return iter(self._items)
    def __contains__(self, x): return x in self._items
    def __eq__(self, o):
        if isinstance(o, array):
            return self.typecode == o.typecode and self._items == o._items
        return False
    def __repr__(self):
        if not self._items:
            return "array('" + self.typecode + "')"
        return "array('" + self.typecode + "', " + repr(self._items) + ")"
    def index(self, x): return self._items.index(x)
    def count(self, x): return self._items.count(x)
    def remove(self, x): self._items.remove(x)
    def pop(self, i=-1): return self._items.pop(i)
    def reverse(self): self._items.reverse()
    def insert(self, i, x):
        if not isinstance(x, self._t): x = self._t(x)
        self._items.insert(i, x)
    def tolist(self): return list(self._items)
    def __add__(self, other):
        if not isinstance(other, array):
            raise TypeError("can only append array (not '%s') to array"
                            % type(other).__name__)
        if other.typecode != self.typecode:
            raise TypeError(
                "bad argument type for built-in operation")
        r = array(self.typecode)
        r._items = list(self._items) + list(other._items)
        return r
    def __iadd__(self, other):
        if not isinstance(other, array):
            raise TypeError("can only extend array (not '%s') with array"
                            % type(other).__name__)
        if other.typecode != self.typecode:
            raise TypeError(
                "bad argument type for built-in operation")
        self._items = list(self._items) + list(other._items)
        return self
    def __mul__(self, n):
        if not isinstance(n, int):
            raise TypeError("can't multiply array by non-int")
        r = array(self.typecode)
        r._items = list(self._items) * n
        return r
    def __rmul__(self, n): return self.__mul__(n)
    def __imul__(self, n):
        if not isinstance(n, int):
            raise TypeError("can't multiply array by non-int")
        self._items = list(self._items) * n
        return self
    def __copy__(self):
        r = array(self.typecode)
        r._items = list(self._items)
        return r
    def __deepcopy__(self, memo):
        r = array(self.typecode)
        r._items = list(self._items)
        return r
    def __reduce_ex__(self, proto):
        return (_array_reconstructor,
                (array, self.typecode, 0, list(self._items)))
    def tobytes(self):
        # Approximation — only valid for byte typecodes.
        if self.typecode in ("b", "B"):
            return bytes(b & 0xFF for b in self._items)
        return b""
    def frombytes(self, b):
        if self.typecode in ("b", "B"):
            for x in b: self.append(x if self.typecode == "B" else (x if x < 128 else x - 256))
    def buffer_info(self): return (0, len(self._items))


ArrayType = array


def _array_reconstructor(arraytype, typecode, mformat_code, items):
    return arraytype(typecode, items)


typecodes = "bBuhHiIlLqQfd"


__all__ = ["array", "ArrayType", "typecodes", "_array_reconstructor"]
