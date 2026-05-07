# pystro stdlib `decimal` — minimal.
#
# Backed by Python int + scale (number of fractional digits).  Enough
# for fixed-precision financial-style arithmetic.  Floats can be lossy
# but are the only literal source.

class Decimal:
    def __init__(self, value=0, scale=None):
        if isinstance(value, Decimal):
            self._n = value._n
            self._s = value._s
            return
        if isinstance(value, str):
            s = value.strip()
            neg = False
            if s.startswith("-"):
                neg = True; s = s[1:]
            elif s.startswith("+"):
                s = s[1:]
            if "." in s:
                w, f = s.split(".")
            else:
                w, f = s, ""
            n = int(w + f) if (w + f) else 0
            if neg: n = -n
            self._n = n
            self._s = len(f)
            return
        if isinstance(value, int):
            self._n = value
            self._s = scale if scale is not None else 0
            return
        # float fallback — best-effort.
        s = repr(value)
        return self.__init__(s)

    def _aligned(self, other):
        a, b = self, other
        if a._s == b._s:
            return a._n, b._n, a._s
        if a._s < b._s:
            return a._n * (10 ** (b._s - a._s)), b._n, b._s
        return a._n, b._n * (10 ** (a._s - b._s)), a._s

    def __add__(self, other):
        if not isinstance(other, Decimal): other = Decimal(other)
        an, bn, s = self._aligned(other)
        r = Decimal(an + bn); r._s = s
        return r
    def __radd__(self, other):
        return Decimal(other) + self
    def __sub__(self, other):
        if not isinstance(other, Decimal): other = Decimal(other)
        an, bn, s = self._aligned(other)
        r = Decimal(an - bn); r._s = s
        return r
    def __rsub__(self, other):
        return Decimal(other) - self
    def __mul__(self, other):
        if not isinstance(other, Decimal): other = Decimal(other)
        r = Decimal(self._n * other._n); r._s = self._s + other._s
        return r
    def __rmul__(self, other):
        return Decimal(other) * self
    def __neg__(self):
        r = Decimal(-self._n); r._s = self._s
        return r
    def __eq__(self, other):
        if not isinstance(other, Decimal): other = Decimal(other)
        an, bn, _ = self._aligned(other)
        return an == bn
    def __lt__(self, other):
        if not isinstance(other, Decimal): other = Decimal(other)
        an, bn, _ = self._aligned(other)
        return an < bn
    def __le__(self, other): return self < other or self == other
    def __gt__(self, other): return not (self <= other)
    def __ge__(self, other): return not (self < other)
    def __hash__(self):
        return hash((self._n, self._s))
    def __repr__(self):
        return "Decimal('" + self._format() + "')"
    def __str__(self):
        return self._format()
    def _format(self):
        n = self._n
        sign = ""
        if n < 0:
            sign = "-"
            n = -n
        s = str(n)
        if self._s == 0:
            return sign + s
        if len(s) <= self._s:
            s = "0" * (self._s - len(s) + 1) + s
        whole = s[:-self._s]
        frac  = s[-self._s:]
        return sign + whole + "." + frac

# Module-level shorthand.
def getcontext():
    return _Context()

class _Context:
    def __init__(self):
        self.prec = 28
