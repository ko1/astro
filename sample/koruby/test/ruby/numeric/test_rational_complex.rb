require_relative "../../test_helper"

def test_rational_basic
  r = Rational(1, 2)
  assert_equal 1, r.numerator
  assert_equal 2, r.denominator
end

def test_rational_addition
  assert_equal Rational(1, 1), Rational(1, 2) + Rational(1, 2)
  assert_equal Rational(7, 12), Rational(1, 3) + Rational(1, 4)
end

def test_rational_subtraction
  assert_equal Rational(1, 6), Rational(1, 2) - Rational(1, 3)
end

def test_rational_multiplication
  assert_equal Rational(1, 6), Rational(1, 2) * Rational(1, 3)
end

def test_rational_division
  assert_equal Rational(3, 2), Rational(1, 2) / Rational(1, 3)
end

def test_rational_int_arith
  assert_equal Rational(3, 2), Rational(1, 2) + 1
  assert_equal Rational(2, 1), Rational(1, 2) * 4
end

def test_rational_to_f
  assert_equal 0.5, Rational(1, 2).to_f
  assert_equal 0.25, Rational(1, 4).to_f
end

def test_rational_literal_1r
  # `1/2r` parses as 1 / (2r) = 1/2
  v = 1 / 2r
  assert_equal Rational(1, 2), v
end

def test_rational_compare
  assert_equal true, Rational(1, 2) < Rational(2, 3)
  assert_equal true, Rational(1, 2) == Rational(2, 4)
end

def test_complex_basic
  c = Complex(2, 3)
  assert_equal 2, c.real
  assert_equal 3, c.imaginary
  assert_equal 3, c.imag
end

def test_complex_addition
  assert_equal Complex(4, 6), Complex(2, 3) + Complex(2, 3)
end

def test_complex_multiplication
  # (2+3i) * (1+i) = 2 + 2i + 3i + 3i^2 = 2 + 5i - 3 = -1 + 5i
  assert_equal Complex(-1, 5), Complex(2, 3) * Complex(1, 1)
end

def test_complex_abs
  # |3+4i| = 5
  assert_equal 5.0, Complex(3, 4).abs
end

def test_complex_conjugate
  assert_equal Complex(2, -3), Complex(2, 3).conjugate
end

def test_imaginary_literal
  v = 5i
  assert_equal Complex(0, 5), v
end

TESTS = [
  :test_rational_basic, :test_rational_addition, :test_rational_subtraction,
  :test_rational_multiplication, :test_rational_division,
  :test_rational_int_arith, :test_rational_to_f,
  :test_rational_literal_1r, :test_rational_compare,
  :test_complex_basic, :test_complex_addition, :test_complex_multiplication,
  :test_complex_abs, :test_complex_conjugate, :test_imaginary_literal,
]
TESTS.each { |t| run_test(t) }
report "RationalComplex"
