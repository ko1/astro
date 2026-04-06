require_relative 'test_helper'

class TestVariables < AbRubyTest
  def test_simple_assign = assert_eval("a = 1; a", 1)
  def test_two_vars = assert_eval("a = 1; b = 2; a + b", 3)
  def test_reassign = assert_eval("a = 1; a = 2; a", 2)
  def test_assign_returns_value = assert_eval("a = 42", 42)
  def test_compound_add = assert_eval("a = 1; a += 2; a", 3)
  def test_compound_sub = assert_eval("a = 10; a -= 3; a", 7)
  def test_compound_mul = assert_eval("a = 5; a *= 3; a", 15)
  def test_scope = assert_eval("a = 1; b = 2; a", 1)
  def test_chain = assert_eval("a = 1; b = a; c = b; c", 1)
end
