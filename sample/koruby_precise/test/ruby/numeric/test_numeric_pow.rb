require_relative "../../test_helper"

# Numeric ** corner cases — Rational/Complex exponents, Float specials.

def test_rational_pow_integer_positive
  assert_equal Rational(8, 27), Rational(2, 3) ** 3
  assert_equal Rational(1, 8),  Rational(1, 2) ** 3
end

def test_rational_pow_zero
  assert_equal Rational(1, 1), Rational(2, 3) ** 0
end

def test_rational_pow_integer_negative
  assert_equal Rational(4, 1), Rational(1, 2) ** -2
  assert_equal Rational(27, 8), Rational(2, 3) ** -3
end

def test_rational_pow_zero_to_negative_raises
  raised = false
  begin
    Rational(0, 1) ** -1
  rescue ZeroDivisionError
    raised = true
  end
  assert raised
end

def test_rational_pow_rational
  # sqrt-ish: Rational(4) ** Rational(1, 2) = 2.0
  assert_equal 2.0, Rational(4, 1) ** Rational(1, 2)
end

def test_rational_pow_float
  assert_equal 4.0, Rational(2, 1) ** 2.0
end

def test_complex_pow_integer
  assert_equal Complex(-7, 24), Complex(3, 4) ** 2
  assert_equal Complex(-117, 44), Complex(3, 4) ** 3
  assert_equal Complex(1, 0),    Complex(3, 4) ** 0
end

def test_complex_pow_negative
  # Complex(2.0, 0) ** -1 = 1/(2+0i) = 0.5+0i.  Use Float reals so
  # Complex#/ falls into Float division (the Integer case rounds to 0).
  c = Complex(2.0, 0.0) ** -1
  assert((c.real - 0.5).abs < 1e-9)
  assert(c.imag.abs < 1e-9)
end

def test_float_nan_predicate
  nan = 0.0 / 0.0
  assert nan.nan?
  assert !1.5.nan?
end

def test_float_infinite_predicate
  assert_equal 1,  Float::INFINITY.infinite?
  assert_equal -1, (-Float::INFINITY).infinite?
  assert_equal nil, 1.0.infinite?
  assert_equal nil, (0.0 / 0.0).infinite?
end

def test_float_finite_predicate
  assert 1.5.finite?
  assert !Float::INFINITY.finite?
  assert !(0.0 / 0.0).finite?
end

def test_float_sign_predicates
  assert 0.0.zero?
  assert !1.5.zero?
  assert 1.5.positive?
  assert !(-1.5).positive?
  assert (-1.5).negative?
  assert !0.0.negative?
end

TESTS = %i[
  test_rational_pow_integer_positive test_rational_pow_zero
  test_rational_pow_integer_negative test_rational_pow_zero_to_negative_raises
  test_rational_pow_rational test_rational_pow_float
  test_complex_pow_integer test_complex_pow_negative
  test_float_nan_predicate test_float_infinite_predicate
  test_float_finite_predicate test_float_sign_predicates
]
TESTS.each {|t| run_test(t) }
report "NumericPow"
