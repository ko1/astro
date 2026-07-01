# Rational#coerce keeps exactness (Integer/Rational -> Rational, Float -> Float),
# and Rational#real? is true. Integer#coerce(Rational) still -> Float. vs ruby.
p Rational(1, 2).coerce(2)
p Rational(1, 2).coerce(Rational(1, 4))
p Rational(1, 2).coerce(0.5)
p Rational(3, 4).coerce(5)
p Rational(1, 2).real?
p 5.coerce(Rational(1, 3))
p (2**70).coerce(Rational(1, 2))
p 5.real?
p 5.0.real?
p Complex(1, 0).real?
p 1 + Rational(1, 2)
p Rational(1, 2) + 1
p 3 * Rational(2, 3)
p [1, Rational(1, 2), 0.25, 2].sort
