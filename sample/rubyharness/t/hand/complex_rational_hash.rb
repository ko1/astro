# Complex#hash and Rational#hash are content-based (equal values hash equal),
# so they work as Hash keys and in uniq. vs ruby.
p Complex(1, 2).hash == Complex(1, 2).hash
p Rational(1, 2).hash == Rational(1, 2).hash
p Rational(2, 4).hash == Rational(1, 2).hash
p Complex(1, 2).hash == Complex(1, 3).hash
h = { Complex(1, 2) => "a", Rational(1, 2) => "b" }
p h[Complex(1, 2)]
p h[Rational(2, 4)]
p [Rational(1, 2), Rational(2, 4), Rational(1, 3)].uniq.size
p [Complex(1, 2), Complex(1, 2), Complex(2, 1)].uniq.size
