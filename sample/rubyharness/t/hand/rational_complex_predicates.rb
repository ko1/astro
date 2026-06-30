# Rational sign predicates + Complex zero?/to_i/to_f/to_r (real-only) — vs ruby.
p Rational(1, 2).negative?
p Rational(-1, 2).positive?
p Rational(-3, 2).negative?
p Rational(3, 2).positive?
p Rational(0, 1).negative?
p Rational(10**40, 3).negative?
p Rational(-(10**40), 3).negative?
p Complex(0, 0).zero?
p Complex(3, 4).zero?
p Complex(3, 0).to_i
p Complex(3, 0).to_f
p Complex(6, 0).to_r
p Complex(0, 0).to_i
begin; Complex(3, 4).to_i; rescue => e; p e.class; end
begin; Complex(3, 4).to_f; rescue => e; p e.class; end
