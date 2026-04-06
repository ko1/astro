require_relative 'test_helper'

class TestInteger < AbRubyTest
  # === Fixnum arithmetic ===
  def test_fix_add = assert_eval("1 + 2", 3)
  def test_fix_sub = assert_eval("10 - 3", 7)
  def test_fix_mul = assert_eval("3 * 4", 12)
  def test_fix_div = assert_eval("10 / 3", 3)
  def test_fix_mod = assert_eval("10 % 3", 1)
  def test_fix_neg = assert_eval("-42", -42)
  def test_fix_pow = assert_eval("2 ** 10", 1024)

  # === Fixnum comparison ===
  def test_fix_lt_true = assert_eval("1 < 2", true)
  def test_fix_lt_false = assert_eval("2 < 1", false)
  def test_fix_le = assert_eval("3 <= 3", true)
  def test_fix_gt = assert_eval("5 > 3", true)
  def test_fix_ge = assert_eval("3 >= 4", false)
  def test_fix_eq_true = assert_eval("42 == 42", true)
  def test_fix_eq_false = assert_eval("1 == 2", false)
  def test_fix_neq = assert_eval("1 != 2", true)

  # === Fixnum methods ===
  def test_fix_zero_true = assert_eval("0.zero?", true)
  def test_fix_zero_false = assert_eval("1.zero?", false)
  def test_fix_abs_pos = assert_eval("42.abs", 42)
  def test_fix_abs_neg = assert_eval("(-42).abs", 42)
  def test_fix_inspect = assert_eval("42.inspect", "42")
  def test_fix_to_s = assert_eval("42.to_s", "42")
  def test_fix_class = assert_eval("42.class", "Integer")

  # === Fixnum overflow → Bignum ===
  def test_add_overflow = assert_eval("4611686018427387903 + 1", 4611686018427387904)
  def test_sub_underflow = assert_eval("-4611686018427387904 - 1", -4611686018427387905)
  def test_mul_overflow = assert_eval("1000000000 * 1000000000", 1000000000000000000)
end
