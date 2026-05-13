# pystro stdlib `math`.  Wraps the C-level `__pystro_*__` primitives.

pi = 3.141592653589793
e  = 2.718281828459045
tau = 6.283185307179586    # 2*pi
inf = float("inf") if False else 1e400        # 1e400 yields inf in IEEE 754
nan = float("nan")

def sqrt(x):  return __pystro_sqrt__(x)
def sin(x):   return __pystro_sin__(x)
def cos(x):   return __pystro_cos__(x)
def tan(x):   return __pystro_tan__(x)
def log(x, base=None):
    if base is None:
        return __pystro_log__(x)
    return __pystro_log__(x, base)
def log2(x):  return __pystro_log__(x, 2)
def log10(x): return __pystro_log__(x, 10)
def exp(x):   return __pystro_exp__(x)
def floor(x): return __pystro_floor__(x)
def ceil(x):  return __pystro_ceil__(x)
def atan2(y, x): return __pystro_atan2__(y, x)
def pow(x, y): return __pystro_pow__(x, y)

def fabs(x):
    # Always returns float (matches Python's math.fabs).
    return float(-x) if x < 0 else float(x)

def hypot(*coords):
    # CPython hypot accepts arbitrarily many coordinates.
    if len(coords) == 0: return 0.0
    s = 0.0
    for c in coords:
        s += c * c
    return __pystro_sqrt__(s)


def dist(p, q):
    # Euclidean distance between two points (iterables of equal length).
    s = 0.0
    pl = list(p)
    ql = list(q)
    if len(pl) != len(ql):
        raise ValueError("both points must have the same number of dimensions")
    for i in range(len(pl)):
        d = pl[i] - ql[i]
        s += d * d
    return __pystro_sqrt__(s)


def fsum(seq):
    # Naive accumulation — CPython's math.fsum is O(n) Kahan/Shewchuk; we
    # approximate with float add (good enough for moderate n).
    s = 0.0
    for x in seq:
        s += x
    return s


def fmod(a, b):
    # C-style remainder: same sign as `a`.
    r = a - (int(a / b) * b)
    return float(r)


def remainder(a, b):
    # IEEE 754 remainder.  Approximation.
    n = round(a / b)
    return float(a - n * b)


def expm1(x):
    return exp(x) - 1.0


def log1p(x):
    return log(1.0 + x)


def cbrt(x):
    if x < 0: return -((-x) ** (1.0 / 3.0))
    return x ** (1.0 / 3.0)

def factorial(n):
    if n < 0:
        raise ValueError("factorial: negative")
    r = 1
    i = 2
    while i <= n:
        r *= i
        i += 1
    return r

def gcd(*args):
    # CPython 3.9+: variadic; gcd() == 0, gcd(x) == |x|.
    if len(args) == 0: return 0
    g = args[0]
    if g < 0: g = -g
    for v in args[1:]:
        a, b = g, v
        if b < 0: b = -b
        while b:
            a, b = b, a % b
        g = a
        if g == 1:
            return 1
    return g

def isnan(x):
    return x != x

def isinf(x):
    return x == inf or x == -inf

def isfinite(x):
    return not (isnan(x) or isinf(x))

def trunc(x):
    # CPython prefers __trunc__ when defined.
    if hasattr(x, "__trunc__"):
        return x.__trunc__()
    if x >= 0: return floor(x)
    return ceil(x)

def copysign(x, y):
    # Use the bit representation to detect the sign of y, so -0.0 is
    # treated as negative (CPython parity).  Float-to-bits is a pystro
    # builtin.
    bits = __pystro_float_to_bits__(float(y))
    if bits >> 63:
        return -fabs(x)
    return fabs(x)

def degrees(x):
    return x * 180.0 / pi

def radians(x):
    return x * pi / 180.0

def sinh(x):
    return (exp(x) - exp(-x)) / 2.0

def cosh(x):
    return (exp(x) + exp(-x)) / 2.0

def tanh(x):
    s = sinh(x)
    c = cosh(x)
    return s / c

def asin(x):
    # crude — pystro lacks asin primitive
    return atan2(x, sqrt(1 - x * x))

def acos(x):
    return atan2(sqrt(1 - x * x), x)

def atan(x):
    return atan2(x, 1)

def lcm(*args):
    if not args: return 1
    r = args[0]
    for a in args[1:]:
        if r == 0 or a == 0: return 0
        g = gcd(r, a)
        if r < 0: r = -r
        if a < 0: a = -a
        r = r // g * a
    return r

def comb(n, k):
    if k < 0 or k > n: return 0
    if k > n - k: k = n - k
    r = 1
    for i in range(k):
        r = r * (n - i) // (i + 1)
    return r

def perm(n, k=None):
    if k is None: k = n
    if k < 0 or k > n: return 0
    r = 1
    for i in range(k):
        r *= (n - i)
    return r

def fmax(a, b):
    """math.fmax — like max() but ignores NaN."""
    if isnan(a): return b
    if isnan(b): return a
    return a if a >= b else b


def fmin(a, b):
    if isnan(a): return b
    if isnan(b): return a
    return a if a <= b else b


def isqrt(n):
    if n < 0:
        raise ValueError("isqrt: negative")
    if n == 0: return 0
    x = n
    y = (x + 1) // 2
    while y < x:
        x = y
        y = (x + n // x) // 2
    return x


def prod(iterable, *, start=1):
    r = start
    for x in iterable: r = r * x
    return r


def floor_div(a, b):  # legacy alias
    return a // b


def isclose(*args, **kwargs):
    """Mimic the C builtin so `class C: isclose = math.isclose` doesn't
    bind it as a method (CPython's math.isclose isn't a descriptor)."""
    # Skip the bound-self leak: when called as `inst.isclose(a, b, ...)`
    # via a class attribute, pystro prepends `inst` as the first arg.
    # Detect the case by checking arg count and types.
    if len(args) >= 3 and not isinstance(args[0], (int, float, complex, bool)):
        # First arg isn't numeric — assume it's a leaked self/cls.
        args = args[1:]
    a = args[0]
    b = args[1]
    rel_tol = args[2] if len(args) >= 3 else kwargs.get("rel_tol", 1e-9)
    abs_tol = args[3] if len(args) >= 4 else kwargs.get("abs_tol", 0.0)
    diff = a - b
    if diff < 0: diff = -diff
    aa = -a if a < 0 else a
    bb = -b if b < 0 else b
    big = aa if aa > bb else bb
    return diff <= big * rel_tol or diff <= abs_tol

__all__ = [
    "pi", "e", "tau", "inf", "nan",
    "sqrt", "sin", "cos", "tan", "asin", "acos", "atan",
    "sinh", "cosh", "tanh",
    "log", "log2", "log10", "exp",
    "floor", "ceil", "trunc", "atan2", "pow", "fabs", "hypot", "dist",
    "factorial", "gcd", "lcm", "comb", "perm",
    "isclose", "isnan", "isinf", "isfinite",
    "copysign", "degrees", "radians",
    "fsum", "fmod", "remainder", "expm1", "log1p", "cbrt",
    "ldexp", "frexp", "modf", "nextafter", "ulp",
]


def ldexp(x, i):
    """x * 2**i."""
    return x * (2 ** i)


def frexp(x):
    """Return (m, e) such that x == m * 2**e and 0.5 <= |m| < 1."""
    if x == 0: return (0.0, 0)
    if isnan(x) or isinf(x): return (x, 0)
    e = 0
    m = float(x)
    if m < 0:
        sign = -1; m = -m
    else:
        sign = 1
    while m >= 1.0: m *= 0.5; e += 1
    while m < 0.5: m *= 2.0; e -= 1
    return (sign * m, e)


def modf(x):
    """Return (frac, int) parts of x as floats."""
    i = float(int(x))
    return (x - i, i)


def nextafter(x, y, steps=1):
    if x == y: return y
    eps = 2.220446049250313e-16
    if y > x: return x + eps * (1 if x == 0 else abs(x))
    return x - eps * (1 if x == 0 else abs(x))


def ulp(x):
    if isnan(x) or isinf(x): return x
    if x == 0: return 5e-324
    return 2.220446049250313e-16 * abs(x)


# Lanczos approximation for the gamma / log-gamma functions.  Accuracy
# is ~14 digits on positive reals; not bit-exact vs CPython but the
# relative error is below test tolerances (math test uses ulp tol).
_LANCZOS_G = 7
_LANCZOS_P = (
    0.99999999999980993,
    676.5203681218851,
    -1259.1392167224028,
    771.32342877765313,
    -176.61502916214059,
    12.507343278686905,
    -0.13857109526572012,
    9.9843695780195716e-6,
    1.5056327351493116e-7,
)


def _lanczos_g(z):
    if z < 0.5:
        return pi / (__pystro_sin__(pi * z) * _lanczos_g(1.0 - z))
    z -= 1.0
    x = _LANCZOS_P[0]
    for i in range(1, len(_LANCZOS_P)):
        x += _LANCZOS_P[i] / (z + i)
    t = z + _LANCZOS_G + 0.5
    return sqrt(2.0 * pi) * (t ** (z + 0.5)) * __pystro_exp__(-t) * x


def gamma(x):
    if isnan(x): return nan
    if x == 0: raise ValueError("math domain error")
    if isinf(x):
        if x > 0: return x
        raise ValueError("math domain error")
    if x == int(x) and x < 0:
        raise ValueError("math domain error")
    return _lanczos_g(float(x))


def lgamma(x):
    if isnan(x): return nan
    if isinf(x): return inf
    if x == int(x) and x <= 0:
        raise ValueError("math domain error")
    g = _lanczos_g(float(x))
    return __pystro_log__(abs(g))


def erf(x):
    # Abramowitz & Stegun 7.1.26 — max abs error ~1.5e-7.
    sign = 1.0 if x >= 0 else -1.0
    ax = abs(float(x))
    if ax > 6: return sign * 1.0
    a1 =  0.254829592
    a2 = -0.284496736
    a3 =  1.421413741
    a4 = -1.453152027
    a5 =  1.061405429
    p  =  0.3275911
    t = 1.0 / (1.0 + p * ax)
    y = 1.0 - (((((a5*t + a4)*t) + a3)*t + a2)*t + a1)*t*__pystro_exp__(-ax*ax)
    return sign * y


def erfc(x):
    return 1.0 - erf(x)


def sumprod(p, q):
    """Return the dot product of two iterables (Python 3.12+)."""
    pi_ = iter(p)
    qi_ = iter(q)
    total = 0
    for a in pi_:
        try:
            b = next(qi_)
        except StopIteration:
            raise ValueError("sumprod: iterables have different lengths")
        total += a * b
    try:
        next(qi_)
    except StopIteration:
        return total
    raise ValueError("sumprod: iterables have different lengths")
