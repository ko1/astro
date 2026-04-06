require_relative 'test_helper'

class TestControlFlow < AbRubyTest
  def test_if_true = assert_eval("if true; 1; else; 2; end", 1)
  def test_if_false = assert_eval("if false; 1; else; 2; end", 2)
  def test_if_nil_falsy = assert_eval("if nil; 1; else; 2; end", 2)
  def test_if_zero_truthy = assert_eval("if 0; 1; else; 2; end", 1)
  def test_if_no_else_true = assert_eval("if true; 42; end", 42)
  def test_if_no_else_false = assert_eval("if false; 42; end", nil)
  def test_if_expr = assert_eval("a = 5; if a > 3; 1; else; 0; end", 1)
  def test_unless_true = assert_eval("unless true; 1; else; 2; end", 2)
  def test_unless_false = assert_eval("unless false; 1; else; 2; end", 1)
  def test_while = assert_eval("a = 0; i = 0; while i < 5; a += 1; i += 1; end; a", 5)
  def test_while_returns_nil = assert_eval("i = 0; while i < 1; i += 1; end", nil)
  def test_while_false = assert_eval("a = 0; while false; a += 1; end; a", 0)
  def test_seq_returns_last = assert_eval("1; 2; 3", 3)
end
