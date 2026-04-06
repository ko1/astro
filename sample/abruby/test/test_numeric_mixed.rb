require_relative 'test_helper'

class TestNumericMixed < AbRubyTest
  # === Fixnum + Float ===
  def test_fix_plus_float = assert_eval("1 + 1.5", 2.5)
  def test_float_plus_fix = assert_eval("1.5 + 1", 2.5)
  def test_fix_mul_float = assert_eval("3 * 1.5", 4.5)
  def test_float_mul_fix = assert_eval("1.5 * 3", 4.5)
  def test_fix_div_float = assert_eval("10 / 2.5", 4.0)
  def test_float_div_fix = assert_eval("7.5 / 3", 2.5)
  def test_fix_sub_float = assert_eval("5 - 1.5", 3.5)
  def test_float_sub_fix = assert_eval("5.5 - 2", 3.5)
  def test_fix_lt_float = assert_eval("1 < 1.5", true)
  def test_float_gt_fix = assert_eval("1.5 > 1", true)
  def test_fix_eq_float = assert_eval("1 == 1.0", true)
  def test_fix_neq_float = assert_eval("1 != 1.5", true)

  # === Bignum + Float ===
  def test_big_plus_float = assert_eval("2 ** 100 + 0.5", 2 ** 100 + 0.5)
  def test_float_plus_big = assert_eval("1.5 + 2 ** 100", 1.5 + 2 ** 100)
  def test_big_mul_float = assert_eval("2 ** 100 * 1.5", 2 ** 100 * 1.5)
  def test_big_gt_float = assert_eval("2 ** 100 > 1.5", true)
  def test_float_lt_big = assert_eval("1.5 < 2 ** 100", true)
  def test_big_eq_float = assert_eval("2 ** 100 == 2.0 ** 100", true)
  def test_big_to_f = assert_eval("(2 ** 100).to_f", (2 ** 100).to_f)

  # === 3-type mixed expressions ===
  def test_fix_float_big = assert_eval("1 + 1.5 + 2 ** 100", 1 + 1.5 + 2 ** 100)
  def test_big_float_fix = assert_eval("a = 2 ** 100; b = 1.5; c = 42; a + b + c", 2**100 + 1.5 + 42)
  def test_float_to_i_plus_big = assert_eval("1.5.to_i + 2 ** 100", 1 + 2 ** 100)
  def test_fix_to_f_plus_big = assert_eval("42.to_f + 2 ** 100", 42.0 + 2 ** 100)

  # === Type coercion in operations ===
  def test_int_div_returns_float = assert_eval("1 / 2.0", 0.5)
  def test_float_pow_int = assert_eval("2.0 ** 10", 1024.0)
  def test_fix_pow_float = assert_eval("4 ** 0.5", 2.0)

  # === Mixed in containers ===
  def test_mixed_array = assert_eval("[1, 1.5, 2 ** 100]", [1, 1.5, 2 ** 100])
  def test_mixed_hash = assert_eval('{1 => 1.5, 2 => 2 ** 100}[1]', 1.5)

  # === Mixed in methods ===
  def test_method_mixed_args = assert_eval(
    "def add(a, b); a + b; end; add(1.5, 2 ** 100)", 1.5 + 2 ** 100)
  def test_method_returns_float = assert_eval(
    "def half(n); n / 2.0; end; half(7)", 3.5)

  # === heap Float (non-Flonum) mixed ===
  def test_fix_plus_heap_float = assert_eval("1 + 1.0e100", 1 + 1.0e100)
  def test_heap_float_plus_fix = assert_eval("1.0e100 + 42", 1.0e100 + 42)
  def test_big_plus_heap_float = assert_eval("2 ** 100 + 1.0e100", 2**100 + 1.0e100)
  def test_heap_float_plus_big = assert_eval("1.0e100 + 2 ** 100", 1.0e100 + 2**100)
  def test_heap_float_mul_flonum = assert_eval("1.0e100 * 1.5", 1.0e100 * 1.5)

  # === 3-type mixed: Fixnum + Bignum + heap Float ===
  def test_fix_big_heap = assert_eval("42 + 2 ** 100 + 1.0e100", 42 + 2**100 + 1.0e100)
  def test_heap_big_fix = assert_eval("1.0e100 + 2 ** 100 + 42", 1.0e100 + 2**100 + 42)

  # === Comparison across types ===
  def test_fix_lt_big = assert_eval("42 < 2 ** 100", true)
  def test_big_gt_fix = assert_eval("2 ** 100 > 42", true)
  def test_float_le_fix = assert_eval("1.0 <= 1", true)
  def test_fix_ge_float = assert_eval("1 >= 1.0", true)
  def test_big_neq_float = assert_eval("2 ** 100 != 1.5", true)
end
