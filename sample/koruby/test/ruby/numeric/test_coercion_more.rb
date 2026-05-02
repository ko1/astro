require_relative "../../test_helper"

# Numeric mixed-type arithmetic and Bignum overflow.

# ---------- Integer + Float ----------

def test_int_plus_float
  assert_equal 3.5, 1 + 2.5
  assert_equal 3.5, 2.5 + 1
  assert_equal 0.5, 1 - 0.5
  assert_equal 5.0, 2.5 * 2
end

def test_int_div_float
  assert_equal 0.5, 1 / 2.0
  assert_equal 2.0, 5 / 2.5
end

def test_int_compare_float
  assert_equal true,  1 < 1.5
  assert_equal true,  2 > 1.5
  assert_equal true,  1 == 1.0
  assert_equal 0,     1 <=> 1.0
end

# ---------- Bignum ----------

def test_fixnum_to_bignum_overflow
  big = 1 << 62
  assert big > 0, "expected positive after overflow into Bignum"
  big2 = big + big
  assert big2 > big, "addition into Bignum should not silently overflow"
end

def test_bignum_arithmetic
  a = 10 ** 20
  b = 10 ** 20
  assert_equal 10 ** 21, a * 10
  assert_equal 0, a - b
end

def test_bignum_coerce_with_float
  big = 10 ** 20
  result = big + 0.5
  assert(result.is_a?(Float), "Bignum + Float should give Float, got #{result.class}")
end

# ---------- Negative modulo (math, not C) ----------

def test_negative_modulo
  assert_equal 2, (-1) % 3       # Ruby: floored mod
  assert_equal 1, (-2) % 3
  assert_equal -1, (-1) % -3
end

TESTS = [
  :test_int_plus_float,
  :test_int_div_float,
  :test_int_compare_float,
  :test_fixnum_to_bignum_overflow,
  :test_bignum_arithmetic,
  :test_bignum_coerce_with_float,
  :test_negative_modulo,
]
TESTS.each { |t| run_test(t) }
report "CoercionMore"
