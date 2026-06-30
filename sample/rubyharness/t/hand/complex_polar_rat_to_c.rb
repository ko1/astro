p Complex(3, 4).polar
p Complex(0, 0).polar
p Complex(1, 1).polar.map { |x| x.round(4) }
p Rational(1, 2).to_c
p Rational(3, 4).to_c
p Rational(1, 2).to_c.real
p Rational(1, 2).to_c.imaginary
p Complex(3, 4).abs
p [Complex(3, 4).abs, Complex(3, 4).arg.round(4)]
p Complex(5, 12).polar.map { |x| x.round(4) }
