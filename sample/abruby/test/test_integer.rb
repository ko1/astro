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

  # === Spaceship ===
  def test_cmp_lt = assert_eval("1 <=> 2", -1)
  def test_cmp_eq = assert_eval("2 <=> 2", 0)
  def test_cmp_gt = assert_eval("3 <=> 2", 1)

  # === Bit operations ===
  def test_lshift = assert_eval("1 << 10", 1024)
  def test_rshift = assert_eval("1024 >> 5", 32)
  def test_band = assert_eval("0xff & 0x0f", 15)
  def test_bor = assert_eval("0x0f | 0xf0", 255)
  def test_bxor = assert_eval("0xff ^ 0x0f", 240)
  def test_bnot = assert_eval("~0", -1)
  def test_bnot_pos = assert_eval("~255", -256)

  # === Fixnum overflow → Bignum ===
  def test_add_overflow = assert_eval("4611686018427387903 + 1", 4611686018427387904)
  def test_sub_underflow = assert_eval("-4611686018427387904 - 1", -4611686018427387905)
  def test_mul_overflow = assert_eval("1000000000 * 1000000000", 1000000000000000000)

  # === Bit indexing ===
  def test_aref_bit0 = assert_eval("5[0]", 1)
  def test_aref_bit1 = assert_eval("5[1]", 0)
  def test_aref_bit2 = assert_eval("5[2]", 1)
  def test_aref_negative_idx = assert_eval("5[-1]", 0)
  def test_aref_negative_val = assert_eval("-1[0]", 1)
  def test_aref_zero = assert_eval("0[0]", 0)
end
