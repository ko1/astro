require_relative 'test_helper'

class TestMethodCall < AbRubyTest
  # function call (no receiver)
  def test_simple_call = assert_eval("def foo; 42; end; foo", 42)
  def test_call_1_arg = assert_eval("def f(a); a; end; f(1)", 1)
  def test_call_2_args = assert_eval("def f(a, b); a + b; end; f(1, 2)", 3)
  def test_call_3_args = assert_eval("def f(a, b, c); a + b + c; end; f(1, 2, 3)", 6)
  def test_call_4_args = assert_eval("def f(a, b, c, d); a + b + c + d; end; f(1, 2, 3, 4)", 10)
  def test_recursion = assert_eval("def fact(n); if n < 2; 1; else; n * fact(n - 1); end; end; fact(5)", 120)
  def test_fib = assert_eval("def fib(n); if n < 2; n; else; fib(n - 1) + fib(n - 2); end; end; fib(10)", 55)
  def test_nested_call = assert_eval("def f(a); a + 1; end; def g(a); a * 2; end; f(g(3))", 7)
  def test_expr_arg = assert_eval("def f(a); a; end; f(1 + 2)", 3)
  def test_call_as_arg = assert_eval("def f(a); a; end; def g(a); a * 10; end; f(g(5))", 50)
  def test_seq_calls = assert_eval("def f(a); a + 1; end; f(1); f(2); f(3)", 4)
  def test_redefine = assert_eval("def f; 1; end; a = f; def f; 2; end; b = f; a + b * 10", 21)
  def test_mutual_recursion = assert_eval(
    "def even(n); if n == 0; true; else; odd(n - 1); end; end; " \
    "def odd(n); if n == 0; false; else; even(n - 1); end; end; even(10)", true)
  def test_empty_method = assert_eval("def f; end; f", nil)
  def test_method_returns_expr = assert_eval("def f(a, b); if a > b; a; else; b; end; end; f(3, 5)", 5)
  def test_overwrite = assert_eval("def f; 1; end; a = f; def f; 2; end; a + f", 3)
  def test_call_results_as_args = assert_eval("def double(x); x * 2; end; def add(a, b); a + b; end; add(double(3), double(4))", 14)
  def test_fib_15 = assert_eval("def fib(n); if n < 2; n; else; fib(n - 1) + fib(n - 2); end; end; fib(15)", 610)
  def test_deep_nesting = assert_eval("def f(x); x + 1; end; def g(x); x * 2; end; def h(x); x - 1; end; f(g(h(10)))", 19)

  # method call on Integer
  def test_int_method = assert_eval("1 + 2", 3)
  def test_int_chain = assert_eval("1 + 2 + 3", 6)
  def test_int_zero_false = assert_eval("42.zero?", false)
  def test_int_zero_true = assert_eval("0.zero?", true)
  def test_int_abs = assert_eval("(-5).abs", 5)
  def test_int_to_s = assert_eval("42.to_s", "42")
  def test_int_class = assert_eval("1.class", "Integer")

  # method call on String
  def test_str_concat = assert_eval('"a" + "b"', "ab")
  def test_str_repeat = assert_eval('"ab" * 3', "ababab")
  def test_str_length = assert_eval('"hello".length', 5)
  def test_str_upcase = assert_eval('"hello".upcase', "HELLO")
  def test_str_reverse = assert_eval('"hello".reverse', "olleh")
  def test_str_empty_false = assert_eval('"x".empty?', false)
  def test_str_empty_true = assert_eval('"".empty?', true)
  def test_str_class = assert_eval('"hello".class', "String")

  # method call on bool/nil
  def test_true_eq = assert_eval("true == true", true)
  def test_true_neq = assert_eval("true != false", true)
  def test_nil_nil_p = assert_eval("nil.nil?", true)
  def test_int_nil_p = assert_eval("42.nil?", false)
  def test_nil_class = assert_eval("nil.class", "NilClass")

  # chained
  def test_chain_methods = assert_eval('"hello".upcase.reverse', "OLLEH")
  def test_chain_with_args = assert_eval('"ab".+("cd").length', 4)
  def test_method_result_as_arg = assert_eval("def f(a); a * 2; end; f(3 + 4)", 14)
  def test_method_result_as_recv = assert_eval("1.+(2).+(3)", 6)
  def test_nested_method_calls = assert_eval("def f(a); a + 10; end; f(2 + 3)", 15)
  def test_binop_in_arg = assert_eval("def f(a, b); a + b; end; f(1 + 2, 3 + 4)", 10)
  def test_call_in_method_arg = assert_eval("def double(x); x * 2; end; 1 + double(3)", 7)
  def test_method_in_call_arg = assert_eval("def f(a); a; end; f(42.to_s)", "42")
  def test_call_with_method_call_arg = assert_eval("def f(a); a + 1; end; f(5.abs)", 6)
  def test_multi_method_args = assert_eval("def f(a, b); a + b; end; f(3.abs, 4.abs)", 7)
  def test_method_in_while = assert_eval("a = 0; i = 10; while i > 0; a += i; i -= 1; end; a", 55)
  def test_method_in_if = assert_eval('a = "hello"; if a.length > 3; true; else; false; end', true)

  # OOP
  def test_class_method = assert_eval("class Foo; def bar; 42; end; end; Foo.new.bar", 42)
  def test_class_method_with_args = assert_eval("class Foo; def add(a, b); a + b; end; end; Foo.new.add(3, 4)", 7)
  def test_self_identity = assert_eval("class Foo; def me; self; end; end; f = Foo.new; f.me == f", true)
  def test_self_method_call = assert_eval("class Foo; def a; 1; end; def b; self.a + 2; end; end; Foo.new.b", 3)
  def test_chained_oop = assert_eval("class Foo; def val; 42; end; end; Foo.new.val.to_s", "42")
  def test_ivar_in_method = assert_eval("class Foo; def initialize(x); @x = x; end; def x; @x; end; end; Foo.new(10).x", 10)
  def test_method_on_other_obj = assert_eval(
    "class Foo; def initialize(v); @v = v; end; def v; @v; end; " \
    "def add_v(other); @v + other.v; end; end; " \
    "a = Foo.new(3); b = Foo.new(4); a.add_v(b)", 7)
  def test_new_multi_args = assert_eval(
    "class P; def initialize(x, y); @x = x; @y = y; end; " \
    "def sum; @x + @y; end; end; P.new(3, 4).sum", 7)
  def test_method_missing = assert_eval(
    'class G; def method_missing(name); "got:" + name; end; end; G.new.hello', "got:hello")
  def test_method_missing_with_arg = assert_eval(
    "class G; def method_missing(name, x); x * 2; end; end; G.new.foo(21)", 42)
  def test_class_two_classes = assert_eval(
    "class A; def val; 10; end; end; class B; def val; 20; end; end; A.new.val + B.new.val", 30)
  def test_interpolation_as_method_result = assert_eval(
    'class Foo; def initialize(n); @n = n; end; def to_s; "Foo(#{@n})"; end; end; Foo.new(42).to_s', "Foo(42)")

  # explicit dispatch
  def test_int_plus_dispatch = assert_eval("1.+(2)", 3)
  def test_int_minus_dispatch = assert_eval("10.-(3)", 7)
  def test_int_mul_dispatch = assert_eval("3.*(4)", 12)
  def test_int_lt_dispatch = assert_eval("1.<(2)", true)
  def test_int_eq_dispatch = assert_eval("3.==(3)", true)
  def test_str_plus_dispatch = assert_eval('"a".+("b")', "ab")
end
