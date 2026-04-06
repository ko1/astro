require_relative 'test_helper'

class TestBignum < AbRubyTest
  # === Bignum literal ===
  def test_big_literal = assert_eval("100000000000000000000", 100000000000000000000)
  def test_big_neg_literal = assert_eval("-100000000000000000000", -100000000000000000000)

  # === Bignum arithmetic ===
  def test_big_add = assert_eval("100000000000000000000 + 1", 100000000000000000001)
  def test_big_sub = assert_eval("100000000000000000000 - 1", 99999999999999999999)
  def test_big_mul = assert_eval("100000000000000000000 * 2", 200000000000000000000)
  def test_big_div = assert_eval("100000000000000000000 / 3", 33333333333333333333)
  def test_big_mod = assert_eval("100000000000000000000 % 7", 100000000000000000000 % 7)
  def test_big_pow = assert_eval("2 ** 100", 2 ** 100)
  def test_big_neg = assert_eval("-(2 ** 100)", -(2 ** 100))

  # === Bignum comparison ===
  def test_big_lt = assert_eval("2 ** 100 < 2 ** 101", true)
  def test_big_gt = assert_eval("2 ** 101 > 2 ** 100", true)
  def test_big_le = assert_eval("2 ** 100 <= 2 ** 100", true)
  def test_big_ge = assert_eval("2 ** 100 >= 2 ** 100", true)
  def test_big_eq_true = assert_eval("2 ** 100 == 2 ** 100", true)
  def test_big_eq_false = assert_eval("2 ** 100 == 2 ** 99", false)
  def test_big_neq = assert_eval("2 ** 100 != 2 ** 99", true)

  # === Bignum methods ===
  def test_big_zero = assert_eval("(2 ** 100).zero?", false)
  def test_big_abs = assert_eval("(-(2 ** 100)).abs", 2 ** 100)
  def test_big_to_s = assert_eval("(2 ** 100).to_s", (2 ** 100).to_s)
  def test_big_inspect = assert_eval("(2 ** 100).inspect", (2 ** 100).to_s)
  def test_big_class = assert_eval("(2 ** 100).class", "Integer")

  # === Fixnum + Bignum mixed ===
  def test_fix_plus_big = assert_eval("1 + 2 ** 100", 1 + 2 ** 100)
  def test_big_plus_fix = assert_eval("2 ** 100 + 1", 2 ** 100 + 1)
  def test_fix_minus_big = assert_eval("0 - 2 ** 100", -(2 ** 100))
  def test_big_minus_fix = assert_eval("2 ** 100 - 1", 2 ** 100 - 1)
  def test_fix_mul_big = assert_eval("3 * (2 ** 100)", 3 * (2 ** 100))
  def test_big_mul_fix = assert_eval("(2 ** 100) * 3", (2 ** 100) * 3)
  def test_big_div_fix = assert_eval("(2 ** 100) / 7", (2 ** 100) / 7)
  def test_fix_lt_big = assert_eval("1 < 2 ** 100", true)
  def test_big_gt_fix = assert_eval("2 ** 100 > 1", true)
  def test_fix_eq_big = assert_eval("1 == 2 ** 100", false)
  def test_big_eq_fix = assert_eval("2 ** 100 == 1", false)

  # === Bignum + Bignum ===
  def test_big_big_add = assert_eval("2 ** 100 + 2 ** 100", 2 ** 101)
  def test_big_big_sub = assert_eval("2 ** 101 - 2 ** 100", 2 ** 100)
  def test_big_big_mul = assert_eval("2 ** 50 * 2 ** 50", 2 ** 100)
  def test_big_big_eq = assert_eval("2 ** 100 + 2 ** 100 == 2 ** 101", true)
  def test_big_big_lt = assert_eval("2 ** 100 < 2 ** 100 + 1", true)

  # === Bignum bit/shift ===
  def test_big_lshift = assert_eval("1 << 100", 1 << 100)
  def test_big_rshift = assert_eval("(2 ** 100) >> 50", (2 ** 100) >> 50)
  def test_big_cmp = assert_eval("(2 ** 100) <=> (2 ** 99)", 1)
  def test_big_cmp_eq = assert_eval("(2 ** 100) <=> (2 ** 100)", 0)

  # === Edge cases ===
  def test_pow_zero = assert_eval("2 ** 0", 1)
  def test_pow_one = assert_eval("2 ** 1", 2)
  def test_big_pow_big = assert_eval("(2 ** 100) ** 2", (2 ** 100) ** 2)
  def test_div_neg = assert_eval("-7 / 2", -7 / 2)
  def test_mod_neg = assert_eval("-7 % 3", -7 % 3)

  # === Bignum in containers ===
  def test_big_in_array = assert_eval("[2 ** 100, 2 ** 200]", [2 ** 100, 2 ** 200])
  def test_big_in_hash = assert_eval('{1 => 2 ** 100}[1]', 2 ** 100)
  def test_big_in_ivar = assert_eval(
    'class TbBig; def initialize(v); @v = v; end; def v; @v; end; end; TbBig.new(2 ** 100).v',
    2 ** 100)

  # === Bignum in loops ===
  def test_big_accumulate = assert_eval(<<~'RUBY', 100 * (2 ** 64))
    a = 0
    i = 0
    while i < 100
      a += 2 ** 64
      i += 1
    end
    a
  RUBY

  def test_factorial = assert_eval(<<~'RUBY', (1..20).reduce(:*))
    def fact(n)
      if n < 2
        1
      else
        n * fact(n - 1)
      end
    end
    fact(20)
  RUBY
end
