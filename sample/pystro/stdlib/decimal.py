# pystro stdlib `decimal` — minimal.
#
# Backed by Python int + scale (number of fractional digits).  Enough
# for fixed-precision financial-style arithmetic.  Floats can be lossy
# but are the only literal source.

# Rounding mode constants — CPython names; pystro's arithmetic uses
# half-even semantics throughout so these are informational.
ROUND_DOWN = "ROUND_DOWN"
ROUND_HALF_UP = "ROUND_HALF_UP"
ROUND_HALF_EVEN = "ROUND_HALF_EVEN"
ROUND_CEILING = "ROUND_CEILING"
ROUND_FLOOR = "ROUND_FLOOR"
ROUND_UP = "ROUND_UP"
ROUND_HALF_DOWN = "ROUND_HALF_DOWN"
ROUND_05UP = "ROUND_05UP"

MAX_PREC = 425000000
MAX_EMAX = 999999999
MIN_EMIN = -999999999


class DecimalException(ArithmeticError):
    pass


class InvalidOperation(DecimalException):
    pass


class Clamped(DecimalException):
    pass


class DivisionByZero(DecimalException, ZeroDivisionError):
    pass


class Inexact(DecimalException):
    pass


class Rounded(DecimalException):
    pass


class Subnormal(DecimalException):
    pass


class Overflow(Inexact, Rounded):
    pass


class Underflow(Inexact, Rounded, Subnormal):
    pass


class FloatOperation(DecimalException, TypeError):
    pass


class DivisionImpossible(InvalidOperation):
    pass


class DivisionUndefined(InvalidOperation, ZeroDivisionError):
    pass


class ConversionSyntax(InvalidOperation):
    pass


class InvalidContext(InvalidOperation):
    pass


class Decimal:
    # Special-form sentinels.  _special is one of '', 'inf', 'nan'.
    def __init__(self, value=0, scale=None):
        self._special = ''
        if isinstance(value, Decimal):
            self._n = value._n
            self._s = value._s
            self._special = value._special
            return
        if isinstance(value, str):
            s = value.strip()
            neg = False
            if s.startswith("-"):
                neg = True; s = s[1:]
            elif s.startswith("+"):
                s = s[1:]
            # Infinity / NaN special forms (CPython parity).
            low = s.lower()
            if low in ("inf", "infinity"):
                self._n = -1 if neg else 1
                self._s = 0
                self._special = 'inf'
                return
            if low == "nan" or low.startswith("nan"):
                self._n = 0
                self._s = 0
                self._special = 'nan'
                return
            # Exponent form: "1.5e2", "1E-3"
            exp = 0
            for sep in ("e", "E"):
                if sep in s:
                    s, esuf = s.split(sep, 1)
                    try:
                        exp = int(esuf)
                    except Exception:
                        raise InvalidOperation("Invalid literal for Decimal: " + repr(value))
                    break
            if "." in s:
                w, f = s.split(".", 1)
            else:
                w, f = s, ""
            digits = w + f
            if digits and not digits.lstrip("0123456789") == "":
                raise InvalidOperation("Invalid literal for Decimal: " + repr(value))
            n = int(digits) if digits else 0
            if neg: n = -n
            # Apply exponent: shift the decimal point.
            scale_after = len(f) - exp
            if scale_after < 0:
                n *= 10 ** (-scale_after)
                scale_after = 0
            self._n = n
            self._s = scale_after
            return
        if isinstance(value, int):
            self._n = value
            self._s = scale if scale is not None else 0
            return
        # float fallback — best-effort.
        s = repr(value)
        return self.__init__(s)

    def is_nan(self): return self._special == 'nan'
    def is_infinite(self): return self._special == 'inf'
    def is_finite(self): return self._special == ''
    def is_signed(self): return self._n < 0 or (self._special == 'inf' and self._n < 0)

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
    def __int__(self):
        if self._special == 'nan' or self._special == 'inf':
            raise ValueError("cannot convert special value to int")
        if self._s == 0:
            return int(self._n)
        if self._s > 0:
            return int(self._n // (10 ** self._s))
        return int(self._n * (10 ** -self._s))
    def __float__(self):
        if self._special == 'nan': return float('nan')
        if self._special == 'inf':
            return float('-inf') if self._n < 0 else float('inf')
        return self._n / (10 ** self._s) if self._s > 0 else float(self._n * (10 ** -self._s))
    def __bool__(self):
        return self._special != '' or self._n != 0
    def __abs__(self):
        r = Decimal(self)
        r._n = -r._n if r._n < 0 else r._n
        return r
    def __pos__(self): return Decimal(self)
    def as_integer_ratio(self):
        """Return (numerator, denominator) per PEP 3101.  Raises
        OverflowError for inf and ValueError for NaN, matching CPython."""
        if self._special == 'inf':
            raise OverflowError("cannot convert Infinity to integer ratio")
        if self._special == 'nan':
            raise ValueError("cannot convert NaN to integer ratio")
        if self._s == 0:
            return (int(self._n), 1)
        if self._s > 0:
            # value = n / 10**s — reduce by gcd
            num, den = self._n, 10 ** self._s
            from math import gcd
            g = gcd(abs(num), den)
            return (num // g, den // g)
        # negative scale: value = n * 10**(-s) — already integer
        return (int(self._n * (10 ** -self._s)), 1)
    @property
    def numerator(self):
        # Only valid for integer-valued Decimals (CPython raises
        # AttributeError for non-integer values).  We surface as ratio.
        return self.as_integer_ratio()[0]
    @property
    def denominator(self):
        return self.as_integer_ratio()[1]
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
        self.rounding = ROUND_HALF_EVEN
        # CPython exposes a flags / traps dict keyed by exception class
        # (InvalidOperation, DivisionByZero, ...); pystro tracks no
        # arithmetic flags so an empty dict suffices.
        self.flags = {}
        self.traps = {}
        self.Emin = -999999
        self.Emax = 999999
        self.capitals = 1
        self.clamp = 0
    def copy(self):
        c = _Context()
        c.prec = self.prec
        c.rounding = self.rounding
        c.flags = dict(self.flags)
        c.traps = dict(self.traps)
        c.Emin = self.Emin
        c.Emax = self.Emax
        c.capitals = self.capitals
        c.clamp = self.clamp
        return c
