# Complex.polar with a zero angle keeps the magnitude's type (integer stays
# integer), imaginary 0.0. vs ruby.
p Complex.polar(5, 0)
p Complex.polar(5.0, 0)
p Complex.polar(3, 0)
p Complex.polar(Rational(1, 2), 0)
p Complex.polar(5)
p Complex.polar(5, 0).imaginary
p Complex.polar(5, 0).real
p Complex.polar(-3, 0)
