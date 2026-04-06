require_relative 'test_helper'

class TestFloat < AbRubyTest
  # literal
  def test_literal = assert_eval("1.5", 1.5)
  def test_zero = assert_eval("0.0", 0.0)
  def test_negative = assert_eval("-3.14", -3.14)

  # arithmetic
  def test_add = assert_eval("1.5 + 2.3", 1.5 + 2.3)
  def test_sub = assert_eval("5.0 - 1.5", 3.5)
  def test_mul = assert_eval("2.5 * 4.0", 10.0)
  def test_div = assert_eval("10.0 / 4.0", 2.5)
  def test_mod = assert_eval("10.0 % 3.0", 10.0 % 3.0)
  def test_pow = assert_eval("2.0 ** 10", 1024.0)
  def test_neg = assert_eval("(-3.14)", -3.14)

  # comparison
  def test_lt = assert_eval("1.5 < 2.0", true)
  def test_lt_false = assert_eval("2.0 < 1.5", false)
  def test_le = assert_eval("1.5 <= 1.5", true)
  def test_gt = assert_eval("2.0 > 1.5", true)
  def test_ge = assert_eval("1.5 >= 1.5", true)
  def test_eq = assert_eval("1.5 == 1.5", true)
  def test_eq_false = assert_eval("1.5 == 2.5", false)
  def test_neq = assert_eval("1.5 != 2.5", true)

  # methods
  def test_abs = assert_eval("(-3.14).abs", 3.14)
  def test_zero_p_true = assert_eval("0.0.zero?", true)
  def test_zero_p_false = assert_eval("1.0.zero?", false)
  def test_floor = assert_eval("3.7.floor", 3)
  def test_ceil = assert_eval("3.2.ceil", 4)
  def test_round = assert_eval("3.5.round", 4)
  def test_to_i = assert_eval("3.14.to_i", 3)
  def test_to_f = assert_eval("3.14.to_f", 3.14)
  def test_to_s = assert_eval("3.14.to_s", "3.14")
  def test_class = assert_eval("1.5.class", "Float")

  # Int + Float mixed
  def test_int_plus_float = assert_eval("1 + 1.5", 2.5)
  def test_float_plus_int = assert_eval("1.5 + 1", 2.5)
  def test_int_mul_float = assert_eval("3 * 1.5", 4.5)
  def test_float_mul_int = assert_eval("1.5 * 3", 4.5)
  def test_int_div_float = assert_eval("10 / 2.5", 4.0)
  def test_int_to_f = assert_eval("42.to_f", 42.0)
  def test_int_lt_float = assert_eval("1 < 1.5", true)
  def test_float_gt_int = assert_eval("1.5 > 1", true)
  def test_int_eq_float = assert_eval("1 == 1.0", true)

  # heap Float (non-Flonum, large values)
  def test_heap_float_literal = assert_eval("1.0e100", 1.0e100)
  def test_heap_float_add = assert_eval("1.0e100 + 1.0e100", 2.0e100)
  def test_heap_float_mul = assert_eval("1.0e100 * 2", 2.0e100)
  def test_heap_float_gt = assert_eval("1.0e100 > 1.5", true)
  def test_heap_float_class = assert_eval("1.0e100.class", "Float")
  def test_heap_float_abs = assert_eval("(-1.0e100).abs", 1.0e100)
  def test_heap_float_to_s = assert_eval("1.0e100.to_s", "1.0e+100")

  # Flonum + heap Float mixed
  def test_flonum_plus_heap = assert_eval("1.5 + 1.0e100", 1.5 + 1.0e100)
  def test_heap_plus_flonum = assert_eval("1.0e100 + 1.5", 1.0e100 + 1.5)

  # Float in containers
  def test_in_array = assert_eval("[1.0, 2.0, 3.0]", [1.0, 2.0, 3.0])
  def test_in_ivar = assert_eval(
    'class TfF; def initialize(v); @v = v; end; def v; @v; end; end; TfF.new(3.14).v', 3.14)
end
