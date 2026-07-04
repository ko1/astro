# Rational ** a huge exponent: base 0 → ZeroDivisionError (neg) / 0 (pos), base
# ±1 → ±1, otherwise ArgumentError (not NotImplementedError). vs ruby.
big = 2 ** 64
begin; Rational(0) ** -big; rescue ZeroDivisionError; p :zdiv; end
p Rational(0) ** big
begin; Rational(2) ** big; rescue ArgumentError; p :arg; end
p Rational(1) ** big
p Rational(-1) ** big
p Rational(-1) ** (big + 1)
p Rational(2) ** 10
p Rational(1, 2) ** 4
