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

def gcd(a, b):
    if a < 0: a = -a
    if b < 0: b = -b
    while b:
        a, b = b, a % b
    return a

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
    if y < 0:
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

def isclose(a, b, rel_tol=1e-9, abs_tol=0.0):
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
