# pystro stdlib `fractions` — minimal Fraction (rational arithmetic).

def _gcd(a, b):
    if a < 0: a = -a
    if b < 0: b = -b
    while b:
        a, b = b, a % b
    return a


class Fraction:
    def __init__(self, numerator=0, denominator=None):
        if denominator is None:
            if isinstance(numerator, Fraction):
                self._n = numerator._n
                self._d = numerator._d
                return
            if isinstance(numerator, int):
                self._n = numerator
                self._d = 1
                return
            if isinstance(numerator, str):
                s = numerator.strip()
                if "/" in s:
                    a, b = s.split("/")
                    self._n = int(a); self._d = int(b)
                else:
                    self._n = int(s); self._d = 1
                self._normalize()
                return
            if isinstance(numerator, float):
                # Use as_integer_ratio.
                n, d = numerator.as_integer_ratio()
                self._n = n; self._d = d
                self._normalize()
                return
            raise TypeError("Fraction: bad numerator type")
        if not isinstance(numerator, int) or not isinstance(denominator, int):
            raise TypeError("Fraction: int args required")
        if denominator == 0:
            raise ZeroDivisionError("Fraction with denominator 0")
        self._n = numerator
        self._d = denominator
        self._normalize()

    def _normalize(self):
        g = _gcd(self._n, self._d) or 1
        self._n //= g
        self._d //= g
        if self._d < 0:
            self._n = -self._n
            self._d = -self._d

    @property
    def numerator(self): return self._n
    @property
    def denominator(self): return self._d

    def _coerce(self, other):
        if isinstance(other, Fraction): return other
        if isinstance(other, int): return Fraction(other)
        return None

    def __add__(self, other):
        o = self._coerce(other)
        if o is None: return NotImplemented
        return Fraction(self._n * o._d + o._n * self._d, self._d * o._d)
    def __radd__(self, other): return self.__add__(other)
    def __sub__(self, other):
        o = self._coerce(other)
        if o is None: return NotImplemented
        return Fraction(self._n * o._d - o._n * self._d, self._d * o._d)
    def __rsub__(self, other):
        o = self._coerce(other)
        if o is None: return NotImplemented
        return o.__sub__(self)
    def __mul__(self, other):
        o = self._coerce(other)
        if o is None: return NotImplemented
        return Fraction(self._n * o._n, self._d * o._d)
    def __rmul__(self, other): return self.__mul__(other)
    def __truediv__(self, other):
        o = self._coerce(other)
        if o is None: return NotImplemented
        if o._n == 0: raise ZeroDivisionError("Fraction division by zero")
        return Fraction(self._n * o._d, self._d * o._n)
    def __rtruediv__(self, other):
        o = self._coerce(other)
        if o is None: return NotImplemented
        return o.__truediv__(self)
    def __neg__(self):
        return Fraction(-self._n, self._d)
    def __abs__(self):
        return Fraction(abs(self._n), self._d)
    def __eq__(self, other):
        o = self._coerce(other)
        if o is None: return False
        return self._n == o._n and self._d == o._d
    def __lt__(self, other):
        o = self._coerce(other)
        if o is None: return NotImplemented
        return self._n * o._d < o._n * self._d
    def __le__(self, other):
        o = self._coerce(other)
        if o is None: return NotImplemented
        return self._n * o._d <= o._n * self._d
    def __gt__(self, other): return not self.__le__(other)
    def __ge__(self, other): return not self.__lt__(other)
    def __hash__(self):
        return hash((self._n, self._d))
    def __repr__(self):
        return "Fraction(" + str(self._n) + ", " + str(self._d) + ")"
    def __str__(self):
        if self._d == 1: return str(self._n)
        return str(self._n) + "/" + str(self._d)


def gcd(a, b):
    return _gcd(a, b)
