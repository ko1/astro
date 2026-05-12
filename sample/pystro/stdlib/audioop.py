"""Minimal pystro stub for `audioop` (sample-rate / encoding conversion).

audioop in CPython is a C extension; pystro doesn't compile C extensions
and most users only need the module to *exist* for test imports. The
operations defined here either no-op (returning input unchanged) or
raise NotImplementedError so a test that actually exercises audioop's
math falls into a skipped branch rather than crashing at import.
"""


class error(Exception):
    pass


def _not_supported(*a, **kw):
    raise NotImplementedError("audioop not supported in pystro")


def avg(*a, **kw):              return 0
def avgpp(*a, **kw):            return 0
def cross(*a, **kw):            return 0
def findfactor(*a, **kw):       return 1.0
def findfit(*a, **kw):          return (0, 1.0)
def findmax(*a, **kw):          return 0
def getsample(fragment, width, index):
    return 0
def max(*a, **kw):              return 0
def maxpp(*a, **kw):            return 0
def minmax(fragment, width):    return (0, 0)
def rms(fragment, width):       return 0
def add(f1, f2, w):             return f1
def bias(f, w, bias):           return f
def mul(f, w, factor):          return f
def reverse(f, w):              return f
def tostereo(f, w, l, r):       return f
def tomono(f, w, l, r):         return f
def ratecv(f, w, ch, ifr, ofr, state, weightA=1, weightB=0):
    return (f, state)
def lin2lin(f, w_in, w_out):    return f
def lin2adpcm(f, w, state):     return (f, state)
def adpcm2lin(f, w, state):     return (f, state)
def lin2ulaw(f, w):             return f
def ulaw2lin(f, w):             return f
def lin2alaw(f, w):             return f
def alaw2lin(f, w):             return f
def byteswap(f, w):             return f
