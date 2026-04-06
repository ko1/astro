require_relative 'test_helper'

class TestLiterals < AbRubyTest
  def test_num_zero = assert_eval("0", 0)
  def test_num_positive = assert_eval("42", 42)
  def test_num_negative = assert_eval("-1", -1)
  def test_true = assert_eval("true", true)
  def test_false = assert_eval("false", false)
  def test_nil = assert_eval("nil", nil)
  def test_empty_string = assert_eval('""', "")
  def test_string = assert_eval('"hello"', "hello")
  def test_self_at_top = assert_eval("self", nil)
end
