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
  def test_class_method = assert_eval("class TmFoo; def bar; 42; end; end; TmFoo.new.bar", 42)
  def test_class_method_with_args = assert_eval("class TmFoo; def add(a, b); a + b; end; end; TmFoo.new.add(3, 4)", 7)
  def test_self_identity = assert_eval("class TmFoo; def me; self; end; end; f = TmFoo.new; f.me == f", true)
  def test_self_method_call = assert_eval("class TmFoo; def a; 1; end; def b; self.a + 2; end; end; TmFoo.new.b", 3)
  def test_chained_oop = assert_eval("class TmFoo; def val; 42; end; end; TmFoo.new.val.to_s", "42")
  def test_ivar_in_method = assert_eval("class TmFoo; def initialize(x); @x = x; end; def x; @x; end; end; TmFoo.new(10).x", 10)
  def test_method_on_other_obj = assert_eval(
    "class TmFoo; def initialize(v); @v = v; end; def v; @v; end; " \
    "def add_v(other); @v + other.v; end; end; " \
    "a = TmFoo.new(3); b = TmFoo.new(4); a.add_v(b)", 7)
  def test_new_multi_args = assert_eval(
    "class TmP; def initialize(x, y); @x = x; @y = y; end; " \
    "def sum; @x + @y; end; end; TmP.new(3, 4).sum", 7)
  def test_method_missing = assert_eval(
    'class TmG; def method_missing(name); "got:" + name; end; end; TmG.new.hello', "got:hello")
  def test_method_missing_with_arg = assert_eval(
    "class TmG; def method_missing(name, x); x * 2; end; end; TmG.new.foo(21)", 42)
  def test_class_two_classes = assert_eval(
    "class TmA; def val; 10; end; end; class TmB; def val; 20; end; end; TmA.new.val + TmB.new.val", 30)
  def test_interpolation_as_method_result = assert_eval(
    'class TmFoo; def initialize(n); @n = n; end; def to_s; "TmFoo(#{@n})"; end; end; TmFoo.new(42).to_s', "TmFoo(42)")

  # explicit dispatch
  def test_int_plus_dispatch = assert_eval("1.+(2)", 3)
  def test_int_minus_dispatch = assert_eval("10.-(3)", 7)
  def test_int_mul_dispatch = assert_eval("3.*(4)", 12)
  def test_int_lt_dispatch = assert_eval("1.<(2)", true)
  def test_int_eq_dispatch = assert_eval("3.==(3)", true)
  def test_str_plus_dispatch = assert_eval('"a".+("b")', "ab")

  # inline cache
  def test_ic_hit = assert_eval(
    "class ICFoo; def val; 42; end; end; f = ICFoo.new; f.val; f.val; f.val", 42)
  def test_ic_invalidation_redef = assert_eval(
    "class ICA; def val; 1; end; end; a = ICA.new; x = a.val; " \
    "class ICA; def val; 2; end; end; x + a.val", 3)
  def test_ic_invalidation_subclass = assert_eval(
    "class ICB; def foo; 1; end; end; class ICC < ICB; end; " \
    "c = ICC.new; x = c.foo; " \
    "class ICC; def foo; 2; end; end; x + c.foo", 3)
  def test_ic_polymorphic = assert_eval(
    "class ICD; def val; 10; end; end; class ICE; def val; 20; end; end; " \
    "d = ICD.new; e = ICE.new; d.val + e.val", 30)
  def test_ic_invalidation_include = assert_eval(
    "class ICBase; def foo; 1; end; end; class ICF < ICBase; end; " \
    "f = ICF.new; x = f.foo; " \
    "module ICM; def foo; 2; end; end; class ICF; include ICM; end; x + f.foo", 3)

  # === node_func_call (implicit self-call optimization) ===

  def test_func_call_no_args = assert_eval("def f; 99; end; f", 99)
  def test_func_call_one_arg = assert_eval("def f(x); x * 2; end; f(21)", 42)
  def test_func_call_two_args = assert_eval("def f(a, b); a - b; end; f(10, 3)", 7)
  def test_func_call_three_args = assert_eval("def f(a, b, c); a + b + c; end; f(1, 2, 3)", 6)
  def test_func_call_nested = assert_eval("def f(x); x + 1; end; def g(x); f(f(x)); end; g(5)", 7)
  def test_func_call_recursive = assert_eval("def f(n); if n < 1; 0; else; n + f(n - 1); end; end; f(10)", 55)
  def test_func_call_mutual = assert_eval(
    "def even(n); if n == 0; true; else; odd(n - 1); end; end; " \
    "def odd(n); if n == 0; false; else; even(n - 1); end; end; " \
    "even(10)", true)
  def test_func_call_in_class = assert_eval(
    "class FC; def a; 1; end; def b; a + 2; end; end; FC.new.b", 3)
  def test_func_call_cfunc = assert_eval(
    "def f; p(42); end; f", 42)
  def test_func_call_method_missing = assert_eval(
    "class FMM; def method_missing(name); \"missing:\" + name; end; " \
    "def test; foo; end; end; FMM.new.test", "missing:foo")
  def test_func_call_with_explicit_self = assert_eval(
    "class FS; def a; 10; end; def b; self.a + a; end; end; FS.new.b", 20)
  def test_func_call_redefine = assert_eval(
    "def f; 1; end; a = f; def f; 2; end; b = f; a + b", 3)

  # === Float arithmetic in complex expressions (arith_fallback arg_index) ===

  def test_float_multi_var_expr = assert_eval(
    "a = 1.5; b = 2.5; c = 3.0; a * b + c", 6.75)
  def test_float_many_locals = assert_eval(
    "a = 1.0; b = 2.0; c = 3.0; d = 4.0; e = 5.0; f = 6.0; " \
    "g = 7.0; h = 8.0; i = 9.0; j = 10.0; " \
    "a + b + c + d + e + f + g + h + i + j", 55.0)
  def test_float_in_method_many_locals = assert_eval(
    "def calc; a = 1.0; b = 2.0; c = 3.0; d = 4.0; e = 5.0; " \
    "f = 6.0; g = 7.0; h = 8.0; i = 9.0; j = 10.0; " \
    "a * b + c * d - e * f + g * h - i * j; end; calc", -50.0)
  def test_float_chained_mul = assert_eval(
    "a = 2.0; b = 3.0; c = 4.0; a * b * c", 24.0)
  def test_float_method_call_in_arith = assert_eval(
    "class P; def initialize(x); @x = x; end; def x; @x; end; end; " \
    "p1 = P.new(3.0); p2 = P.new(4.0); p1.x * p1.x + p2.x * p2.x", 25.0)
  def test_float_nbody_pattern = assert_eval(
    "class B; def initialize(v, m); @v = v; @m = m; end; " \
    "def v; @v; end; def m; @m; end; def set_v(nv); @v = nv; end; end; " \
    "b = B.new(1.0, 2.0); b.set_v(b.v + 0.5 * b.m * 0.01); b.v", 1.01)
  def test_float_division_chain = assert_eval(
    "dt = 0.01; dsq = 25.0; dist = 5.0; dt / (dsq * dist)", 8.0e-05)
  def test_float_pow = assert_eval("4.0 ** 0.5", 2.0)
  def test_float_comparison_chain = assert_eval(
    "a = 1.5; b = 2.5; c = 3.5; a < b && b < c", true)

  # === Mixed type arithmetic ===

  def test_int_plus_float = assert_eval("1 + 1.5", 2.5)
  def test_float_plus_int = assert_eval("1.5 + 1", 2.5)
  def test_int_mul_float = assert_eval("3 * 2.5", 7.5)
  def test_float_mul_int = assert_eval("2.5 * 3", 7.5)
  def test_int_div_float = assert_eval("1 / 2.0", 0.5)
  def test_int_lt_float = assert_eval("1 < 1.5", true)
  def test_float_lt_int = assert_eval("0.5 < 1", true)
  def test_int_eq_float = assert_eval("1 == 1.0", true)
  def test_mixed_complex_expr = assert_eval(
    "a = 3; b = 1.5; c = 2; a * b + c", 6.5)

  # === method_cache body/dispatcher caching ===

  def test_cache_hit_repeated = assert_eval(
    "class CH; def f; 1; end; end; o = CH.new; " \
    "o.f + o.f + o.f + o.f + o.f", 5)
  def test_cache_across_instances = assert_eval(
    "class CI; def f; 42; end; end; CI.new.f + CI.new.f", 84)
  def test_cache_cfunc_repeated = assert_eval(
    "a = [1, 2, 3]; a.length + a.length", 6)
  def test_cache_after_redefine = assert_eval(
    "class CR; def f; 1; end; end; o = CR.new; x = o.f; " \
    "class CR; def f; 10; end; end; x + o.f", 11)
  def test_cache_inherited_then_override = assert_eval(
    "class CP; def f; 1; end; end; class CQ < CP; end; " \
    "q = CQ.new; x = q.f; class CQ; def f; 2; end; end; x + q.f", 3)

  # === Operator assignment with various types ===

  def test_pluseq_float = assert_eval("a = 1.0; a += 0.5; a", 1.5)
  def test_minuseq_float = assert_eval("a = 3.0; a -= 1.5; a", 1.5)
  def test_muleq_float = assert_eval("a = 2.0; a *= 3.0; a", 6.0)
  def test_pluseq_in_loop = assert_eval(
    "a = 0.0; i = 0; while i < 10; a += 1.5; i += 1; end; a", 15.0)
  def test_pluseq_string = assert_eval('a = "hello"; a += " world"; a', "hello world")
end
