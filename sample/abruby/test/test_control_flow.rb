require_relative 'test_helper'

class TestControlFlow < AbRubyTest
  # if
  def test_if_true = assert_eval("if true; 1; else; 2; end", 1)
  def test_if_false = assert_eval("if false; 1; else; 2; end", 2)
  def test_if_nil_falsy = assert_eval("if nil; 1; else; 2; end", 2)
  def test_if_zero_truthy = assert_eval("if 0; 1; else; 2; end", 1)
  def test_if_no_else_true = assert_eval("if true; 42; end", 42)
  def test_if_no_else_false = assert_eval("if false; 42; end", nil)
  def test_if_expr = assert_eval("a = 5; if a > 3; 1; else; 0; end", 1)

  # elsif
  def test_elsif_first = assert_eval("if true; 1; elsif true; 2; else; 3; end", 1)
  def test_elsif_second = assert_eval("if false; 1; elsif true; 2; else; 3; end", 2)
  def test_elsif_else = assert_eval("if false; 1; elsif false; 2; else; 3; end", 3)
  def test_elsif_chain = assert_eval("a = 2; if a == 1; 10; elsif a == 2; 20; elsif a == 3; 30; else; 0; end", 20)

  # unless
  def test_unless_true = assert_eval("unless true; 1; else; 2; end", 2)
  def test_unless_false = assert_eval("unless false; 1; else; 2; end", 1)

  # postfix if/unless
  def test_postfix_if_true = assert_eval("a = 0; a = 42 if true; a", 42)
  def test_postfix_if_false = assert_eval("a = 0; a = 42 if false; a", 0)
  def test_postfix_unless_true = assert_eval("a = 0; a = 42 unless true; a", 0)
  def test_postfix_unless_false = assert_eval("a = 0; a = 42 unless false; a", 42)

  # ternary
  def test_ternary_true = assert_eval("true ? 1 : 2", 1)
  def test_ternary_false = assert_eval("false ? 1 : 2", 2)
  def test_ternary_expr = assert_eval("(3 > 2) ? 10 : 20", 10)

  # while
  def test_while = assert_eval("a = 0; i = 0; while i < 5; a += 1; i += 1; end; a", 5)
  def test_while_returns_nil = assert_eval("i = 0; while i < 1; i += 1; end", nil)
  def test_while_false = assert_eval("a = 0; while false; a += 1; end; a", 0)

  # until
  def test_until_basic = assert_eval("i = 5; until i == 0; i -= 1; end; i", 0)
  def test_until_never = assert_eval("a = 0; until true; a += 1; end; a", 0)
  def test_until_count = assert_eval("a = 0; i = 0; until i >= 10; a += i; i += 1; end; a", 45)

  # && / ||
  def test_and_true_true = assert_eval("true && true", true)
  def test_and_true_false = assert_eval("true && false", false)
  def test_and_false_skip = assert_eval("false && 42", false)
  def test_and_value = assert_eval("1 && 2", 2)
  def test_and_nil = assert_eval("nil && 42", nil)
  def test_and_chain = assert_eval("1 && 2 && 3", 3)
  def test_or_true_skip = assert_eval("true || 42", true)
  def test_or_false_right = assert_eval("false || 42", 42)
  def test_or_nil_right = assert_eval("nil || 42", 42)
  def test_or_value = assert_eval("1 || 2", 1)
  def test_or_chain = assert_eval("nil || false || 3", 3)
  def test_or_default = assert_eval("a = nil; b = a || 99; b", 99)
  def test_and_or_mixed = assert_eval("nil || true && 42", 42)

  # ! / not
  def test_not_true = assert_eval("!true", false)
  def test_not_false = assert_eval("!false", true)
  def test_not_nil = assert_eval("!nil", true)
  def test_not_truthy = assert_eval("!42", false)
  def test_not_keyword = assert_eval("not true", false)
  def test_not_keyword_false = assert_eval("not false", true)

  # seq
  def test_seq_returns_last = assert_eval("1; 2; 3", 3)

  # return
  def test_return_value
    assert_eval("def f; return 42; end; f", 42)
  end

  def test_return_nil
    assert_eval("def f; return; end; f", nil)
  end

  def test_return_early
    assert_eval("def f; return 1; 2; end; f", 1)
  end

  def test_return_in_if
    assert_eval("def f(x); if x > 0; return 1; end; return 2; end; f(5)", 1)
  end

  def test_return_in_if_else
    assert_eval("def f(x); if x > 0; return 1; else; return 2; end; end; f(-1)", 2)
  end

  def test_return_in_while
    assert_eval("def f; i = 0; while i < 10; if i == 5; return i; end; i += 1; end; end; f", 5)
  end

  def test_return_does_not_affect_caller
    assert_eval("def f; return 10; end; f; 20", 20)
  end

  def test_return_nested_method
    assert_eval("def inner; return 3; end; def outer; return inner + 1; end; outer", 4)
  end

  def test_return_with_expression
    assert_eval("def f(a, b); return a + b; end; f(3, 4)", 7)
  end
end
