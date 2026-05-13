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
    def __float__(self):
        return self._n / self._d
    def __int__(self):
        # Truncate toward zero.
        if (self._n < 0) != (self._d < 0):
            return -(abs(self._n) // abs(self._d))
        return self._n // self._d
    def __index__(self):
        if self._d != 1:
            raise TypeError("Fraction with non-unit denominator is not an integer")
        return self._n
    def __bool__(self):
        return self._n != 0
    def __floor__(self):
        return self._n // self._d
    def __ceil__(self):
        n, d = self._n, self._d
        return -(-n // d)
    def __trunc__(self):
        return self.__int__()
    def __floordiv__(self, other):
        o = self._coerce(other)
        if o is None: return NotImplemented
        return (self._n * o._d) // (o._n * self._d)
    def __mod__(self, other):
        q = self.__floordiv__(other)
        if q is NotImplemented: return NotImplemented
        return self - Fraction(q) * other
    def __divmod__(self, other):
        q = self.__floordiv__(other)
        if q is NotImplemented: return NotImplemented
        return (q, self - Fraction(q) * other)
    def __pow__(self, other):
        if isinstance(other, int):
            if other >= 0:
                return Fraction(self._n ** other, self._d ** other)
            return Fraction(self._d ** -other, self._n ** -other)
        return (self._n / self._d) ** other
    def __pos__(self):
        return self
    def as_integer_ratio(self):
        return (self._n, self._d)
    def limit_denominator(self, max_denominator=1000000):
        # Simplified implementation — just return self if denominator already
        # within the cap; otherwise truncate via continued-fraction approximation.
        if self._d <= max_denominator: return self
        # Stein-Brocot continued fraction approx.
        p0, q0, p1, q1 = 0, 1, 1, 0
        n, d = self._n, self._d
        while True:
            a = n // d
            q2 = q0 + a * q1
            if q2 > max_denominator: break
            p0, q0, p1, q1 = p1, q1, p0 + a * p1, q2
            n, d = d, n - a * d
            if d == 0: break
        k = (max_denominator - q0) // q1 if q1 else 0
        bound1 = Fraction(p0 + k * p1, q0 + k * q1)
        bound2 = Fraction(p1, q1)
        # Pick the closer.
        if abs(bound2 - self) <= abs(bound1 - self):
            return bound2
        return bound1


def gcd(a, b):
    return _gcd(a, b)
