xs = [Complex(1, 0), Complex(1, 2), Complex(3, -4), Complex(1.5, 2), Complex(Rational(1, 3), 2),
      Complex(0, 0), Complex(-0.0, 0.0), Complex(2, 0.0), Complex(1e300, 1e300)]
ys = [Complex(10.067661995777765, 0.0), Complex(1, 2), Complex(3, 4), Complex(0.5, -2), Complex(Rational(2, 3), 1),
      Complex(0, 1), Complex(2, 0), Complex(1e-300, 1e300), Complex(-0.0, 1.0)]
xs.each { |x| ys.each { |y| r = (x / y rescue $!.class); print r.inspect, " " } ; puts }
p 1 / Complex(10.067661995777765, 0.0), 2.0 / Complex(1, 1), Rational(1, 2) / Complex(1, 1)
p((Complex(1, 2) / Complex(0, 0) rescue $!.class), Complex(1.0, 2) / Complex(0, 0), Complex(1, 2).quo(Complex(3, 4)))
p((Math::E ** Complex(0, Math::PI) rescue $!), (2.0 ** Complex(1, 1) rescue $!), (2 ** Complex(1, 1) rescue $!))
p((Rational(1, 2) ** Complex(1, 1) rescue $!), (2 ** Complex(2, 0) rescue $!), (2.0 ** Complex(2, 0) rescue $!))
p((-8.0) ** Rational(1, 3), 4.0 ** Rational(1, 2), (-8) ** Rational(1, 3))
