# Complex#finite?/infinite?/real? and Complex#coerce. vs ruby.
p Complex(3, 4).finite?
p Complex(1.0 / 0, 0).finite?
p Complex(0, 1.0 / 0).finite?
p Complex(3, 4).infinite?
p Complex(1.0 / 0, 0).infinite?
p Complex(3, 4).real?
p Complex(1, 0).real?
p Complex(3, 4).coerce(1)
p Complex(3, 4).coerce(2.5)
p Complex(3, 4).coerce(Complex(1, 1))
p Complex(3, 4).coerce(Rational(1, 2))
p 1 + Complex(2, 3)
p Complex(2, 3) + 1
p 2 * Complex(1, 1)
begin; Complex(3, 4).coerce("x"); rescue => e; p e.class; end
