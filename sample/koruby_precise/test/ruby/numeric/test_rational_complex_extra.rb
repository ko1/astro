require_relative "../../test_helper"

# Rational/Complex coerce + polar/coerce edge cases.

def test_rational_with_bignum
  big = 10 ** 30
  r = Rational(big, 3)
  assert_equal big, r.numerator
  assert_equal 3,   r.denominator

  r2 = Rational(1, 2) + big
  assert_equal big * 2 + 1, r2.numerator
  assert_equal 2, r2.denominator
end

def test_float_plus_rational
  # Reverse direction — Float on LHS, Rational on RHS.
  assert_equal 2.0, 1.5 + Rational(1, 2)
  assert_equal 1.0, 1.5 - Rational(1, 2)
  assert_equal 0.75, 1.5 * Rational(1, 2)
end

def test_rational_plus_float
  assert_equal 2.0, Rational(1, 2) + 1.5
end

def test_complex_polar_form
  c = Complex(3, 4)
  assert_equal 5.0, c.abs
  assert_equal 25,  c.abs2
  pol = c.polar
  assert_equal 5.0, pol[0]
  # angle for (3,4) is atan2(4,3) ≈ 0.927
  assert(pol[1] > 0.92 && pol[1] < 0.93)
end

def test_complex_polar_constructor
  c = Complex.polar(5, 0)
  assert_equal 5.0, c.real
  assert_equal 0.0, c.imaginary
end

def test_complex_rectangular
  c = Complex(3, 4)
  assert_equal [3, 4], c.rect
  assert_equal [3, 4], c.rectangular
end

def test_integer_plus_complex
  c = 1 + Complex(2, 3)
  assert(c.is_a?(Complex))
  assert_equal 3, c.real
  assert_equal 3, c.imag
end

def test_float_plus_complex
  c = 1.5 + Complex(0, 1)
  assert_equal 1.5, c.real
  assert_equal 1,   c.imag
end

def test_rational_plus_complex
  # Rational + Complex via Complex's coerce.
  c = Complex(2, 3) + Rational(1, 2)
  assert_equal Rational(5, 2), c.real
  assert_equal 3, c.imag
end

def test_complex_arg
  assert_equal 0.0, Complex(1, 0).arg
  assert(Complex(0, 1).arg > 1.57 && Complex(0, 1).arg < 1.58)  # ~ π/2
end

def test_complex_conj_abs
  c = Complex(3, -4)
  assert_equal Complex(3, 4),  c.conj
  assert_equal 5.0, c.abs
end

TESTS = %i[
  test_rational_with_bignum test_float_plus_rational
  test_rational_plus_float test_complex_polar_form
  test_complex_polar_constructor test_complex_rectangular
  test_integer_plus_complex test_float_plus_complex
  test_rational_plus_complex test_complex_arg test_complex_conj_abs
]
TESTS.each {|t| run_test(t) }
report "RationalComplexExtra"
