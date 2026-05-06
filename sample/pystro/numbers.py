"""pystro stdlib `numbers` (PEP 3141 ABCs).

Pystro doesn't run a real ABC machinery, so each class lazily registers
the corresponding built-in via __subclasshook__ + register().  The
class hierarchy mirrors CPython:

  Number ⊃ Complex ⊃ Real ⊃ Rational ⊃ Integral
"""

from abc import ABCMeta


class Number(metaclass=ABCMeta):
    __slots__ = ()


class Complex(Number):
    __slots__ = ()


class Real(Complex):
    __slots__ = ()


class Rational(Real):
    __slots__ = ()


class Integral(Rational):
    __slots__ = ()


# Register concrete built-ins on every level they belong to.  Pystro
# doesn't walk the virtual subclass tree, so we register at each parent
# explicitly.
for _cls in (complex, float, int, bool):
    Number.register(_cls)
    Complex.register(_cls)

for _cls in (float, int, bool):
    Real.register(_cls)

for _cls in (int, bool):
    Rational.register(_cls)
    Integral.register(_cls)
del _cls


__all__ = ["Number", "Complex", "Real", "Rational", "Integral"]
