# pystro stdlib `cmath` (minimal): complex math helpers.
import math as _math

pi = _math.pi
e  = _math.e
tau = _math.tau
inf = _math.inf
nan = _math.nan
infj = complex(0, _math.inf)
nanj = complex(0, _math.nan)


def phase(z):
    """Return atan2(z.imag, z.real)."""
    if not isinstance(z, complex):
        z = complex(z, 0)
    return _math.atan2(z.imag, z.real)


def polar(z):
    """Return (r, phi)."""
    if not isinstance(z, complex):
        z = complex(z, 0)
    r = _math.sqrt(z.real * z.real + z.imag * z.imag)
    return (r, _math.atan2(z.imag, z.real))


def rect(r, phi):
    return complex(r * _math.cos(phi), r * _math.sin(phi))


def sqrt(z):
    if not isinstance(z, complex):
        if z >= 0:
            return complex(_math.sqrt(z), 0)
        return complex(0, _math.sqrt(-z))
    r, phi = polar(z)
    return rect(_math.sqrt(r), phi / 2)


def exp(z):
    if not isinstance(z, complex):
        return complex(_math.exp(z), 0)
    er = _math.exp(z.real)
    return complex(er * _math.cos(z.imag), er * _math.sin(z.imag))


def log(z, base=None):
    if not isinstance(z, complex):
        z = complex(z, 0)
    r, phi = polar(z)
    main = complex(_math.log(r), phi)
    if base is None:
        return main
    return main / log(base)


def log10(z):
    return log(z, 10)


def isfinite(z):
    if not isinstance(z, complex):
        return _math.isfinite(z)
    return _math.isfinite(z.real) and _math.isfinite(z.imag)


def isinf(z):
    if not isinstance(z, complex):
        return _math.isinf(z)
    return _math.isinf(z.real) or _math.isinf(z.imag)


def isnan(z):
    if not isinstance(z, complex):
        return _math.isnan(z)
    return _math.isnan(z.real) or _math.isnan(z.imag)


def isclose(a, b, *, rel_tol=1e-9, abs_tol=0.0):
    if not isinstance(a, complex):
        a = complex(a, 0)
    if not isinstance(b, complex):
        b = complex(b, 0)
    diff = a - b
    diff_mag = _math.sqrt(diff.real * diff.real + diff.imag * diff.imag)
    a_mag = _math.sqrt(a.real * a.real + a.imag * a.imag)
    b_mag = _math.sqrt(b.real * b.real + b.imag * b.imag)
    tol = max(rel_tol * max(a_mag, b_mag), abs_tol)
    return diff_mag <= tol


def sin(z):
    if not isinstance(z, complex):
        return complex(_math.sin(z), 0)
    return complex(_math.sin(z.real) * _math.cosh(z.imag),
                   _math.cos(z.real) * _math.sinh(z.imag))


def cos(z):
    if not isinstance(z, complex):
        return complex(_math.cos(z), 0)
    return complex(_math.cos(z.real) * _math.cosh(z.imag),
                  -_math.sin(z.real) * _math.sinh(z.imag))


def tan(z):
    s = sin(z)
    c = cos(z)
    return s / c


# Inverse / hyperbolic trig — algebraic identities over complex log/sqrt.
def acos(z):
    if not isinstance(z, complex): z = complex(z, 0)
    return -1j * log(z + 1j * sqrt(1 - z * z))


def asin(z):
    if not isinstance(z, complex): z = complex(z, 0)
    return -1j * log(1j * z + sqrt(1 - z * z))


def atan(z):
    if not isinstance(z, complex): z = complex(z, 0)
    return 0.5j * (log(1 - 1j * z) - log(1 + 1j * z))


def sinh(z):
    if not isinstance(z, complex): z = complex(z, 0)
    return -1j * sin(1j * z)


def cosh(z):
    if not isinstance(z, complex): z = complex(z, 0)
    return cos(1j * z)


def tanh(z):
    return sinh(z) / cosh(z)


def acosh(z):
    if not isinstance(z, complex): z = complex(z, 0)
    return log(z + sqrt(z * z - 1))


def asinh(z):
    if not isinstance(z, complex): z = complex(z, 0)
    return log(z + sqrt(z * z + 1))


def atanh(z):
    if not isinstance(z, complex): z = complex(z, 0)
    return 0.5 * (log(1 + z) - log(1 - z))


__all__ = ["phase", "polar", "rect", "sqrt", "exp", "log", "log10",
           "isfinite", "isinf", "isnan", "isclose",
           "sin", "cos", "tan", "asin", "acos", "atan",
           "sinh", "cosh", "tanh", "asinh", "acosh", "atanh",
           "pi", "e", "tau", "inf", "nan", "infj", "nanj"]
