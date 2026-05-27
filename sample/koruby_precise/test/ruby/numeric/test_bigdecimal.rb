require_relative "../../test_helper"

# BigDecimal — Rational-backed arbitrary precision decimal.

def test_construct_from_string
  b = BigDecimal("3.14")
  assert_equal "3.14", b.to_s
  assert_equal 3, b.to_i
end

def test_construct_from_integer
  b = BigDecimal(42)
  assert_equal "42.0", b.to_s
  assert_equal 42, b.to_i
end

def test_helper_function
  b = BigDecimal("1.5")
  assert b.is_a?(BigDecimal)
  assert_equal 1.5, b.to_f
end

def test_negative
  b = BigDecimal("-2.5")
  assert b.negative?
  assert_equal "-2.5", b.to_s
end

def test_zero_predicates
  assert BigDecimal("0").zero?
  assert !BigDecimal("0.001").zero?
end

def test_addition
  assert_equal BigDecimal("4"), BigDecimal("3.14") + BigDecimal("0.86")
end

def test_subtraction
  assert_equal BigDecimal("1"), BigDecimal("3.14") - BigDecimal("2.14")
end

def test_multiplication
  assert_equal BigDecimal("314"), BigDecimal("3.14") * 100
end

def test_division
  # Exact: 3.14 / 2 = 1.57
  assert_equal BigDecimal("1.57"), BigDecimal("3.14") / 2
end

def test_decimal_precision_avoids_float_drift
  # Float: 0.1 + 0.2 = 0.30000000000000004 — BigDecimal stays exact.
  result = BigDecimal("0.1") + BigDecimal("0.2")
  assert_equal BigDecimal("0.3"), result
  assert_equal "0.3", result.to_s
end

def test_division_by_zero_yields_infinity
  inf = BigDecimal("1") / BigDecimal("0")
  assert_equal 1, inf.infinite?
  ninf = BigDecimal("-1") / BigDecimal("0")
  assert_equal -1, ninf.infinite?
  nan = BigDecimal("0") / BigDecimal("0")
  assert nan.nan?
end

def test_special_values
  assert BigDecimal("NaN").nan?
  assert_equal 1,  BigDecimal("Infinity").infinite?
  assert_equal -1, BigDecimal("-Infinity").infinite?
  assert BigDecimal("3.14").finite?
end

def test_comparison
  a = BigDecimal("3.14")
  b = BigDecimal("3.15")
  assert a < b
  assert b > a
  assert a == BigDecimal("3.14")
  assert a != b
end

def test_comparison_with_integer
  assert BigDecimal("5") == 5
  assert BigDecimal("5.5") > 5
  assert BigDecimal("4.5") < 5
end

def test_abs
  assert_equal BigDecimal("3.14"), BigDecimal("-3.14").abs
  assert_equal BigDecimal("3.14"), BigDecimal("3.14").abs
end

def test_unary_minus
  assert_equal BigDecimal("-3.14"), -BigDecimal("3.14")
end

def test_pow_integer
  assert_equal BigDecimal("9"), BigDecimal("3") ** 2
end

def test_to_f_special
  assert_equal Float::INFINITY,  BigDecimal("Infinity").to_f
  assert_equal -Float::INFINITY, BigDecimal("-Infinity").to_f
  assert BigDecimal("NaN").to_f.nan?
end

def test_construct_from_scientific
  assert_equal BigDecimal("314"),    BigDecimal("3.14e2")
  assert_equal BigDecimal("0.0314"), BigDecimal("3.14e-2")
end

TESTS = %i[
  test_construct_from_string test_construct_from_integer
  test_helper_function test_negative test_zero_predicates
  test_addition test_subtraction test_multiplication test_division
  test_decimal_precision_avoids_float_drift
  test_division_by_zero_yields_infinity test_special_values
  test_comparison test_comparison_with_integer
  test_abs test_unary_minus test_pow_integer
  test_to_f_special test_construct_from_scientific
]
TESTS.each {|t| run_test(t) }
report "BigDecimal"
