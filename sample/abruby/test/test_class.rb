require_relative 'test_helper'

class TestClass < AbRubyTest
  def test_empty_class = assert_eval("class TcFoo; end; nil", nil)
  def test_class_def_call = assert_eval("class TcFoo; def bar; 42; end; end; TcFoo.new.bar", 42)
  def test_init_no_args = assert_eval("class TcFoo; def initialize; @x = 99; end; def x; @x; end; end; TcFoo.new.x", 99)
  def test_init_1_arg = assert_eval("class TcFoo; def initialize(v); @v = v; end; def v; @v; end; end; TcFoo.new(42).v", 42)
  def test_init_2_args = assert_eval("class TcP; def initialize(x, y); @x = x; @y = y; end; def sum; @x + @y; end; end; TcP.new(3, 4).sum", 7)
  def test_reopen = assert_eval("class TcFoo; def a; 1; end; end; class TcFoo; def b; 2; end; end; TcFoo.new.a + TcFoo.new.b", 3)
  def test_multiple_instances = assert_eval("class TcC; def initialize(v); @v = v; end; def v; @v; end; end; a = TcC.new(10); b = TcC.new(20); a.v + b.v", 30)
  def test_instance_independence = assert_eval("class TcC; def initialize(v); @v = v; end; def v; @v; end; end; a = TcC.new(1); b = TcC.new(2); c = TcC.new(3); a.v + b.v + c.v", 6)
  def test_ivar_default_nil = assert_eval("class TcC; def x; @x; end; end; TcC.new.x", nil)
  def test_ivar_mutation = assert_eval("class TcC; def initialize; @x = 0; end; def inc; @x += 1; end; def x; @x; end; end; c = TcC.new; c.inc; c.inc; c.inc; c.x", 3)
  def test_method_override = assert_eval("class TcFoo; def val; 1; end; end; a = TcFoo.new.val; class TcFoo; def val; 2; end; end; a + TcFoo.new.val", 3)
  def test_custom_inspect = assert_eval('class TcFoo; def inspect; "hello"; end; end; TcFoo.new.inspect', "hello")
  def test_self_method_dispatch = assert_eval("class TcCalc; def double(x); x * 2; end; def quad(x); self.double(self.double(x)); end; end; TcCalc.new.quad(3)", 12)
  def test_class_operator = assert_eval("class TcVec; def initialize(x); @x = x; end; def +(other); TcVec.new(@x + other.x); end; def x; @x; end; end; a = TcVec.new(3); b = TcVec.new(4); c = a + b; c.x", 7)
  def test_user_obj_eq_identity = assert_eval("class TcFoo; end; a = TcFoo.new; a == a", true)
  def test_user_obj_eq_different = assert_eval("class TcFoo; end; TcFoo.new == TcFoo.new", false)
  def test_user_obj_neq = assert_eval("class TcFoo; end; TcFoo.new != TcFoo.new", true)
  def test_user_obj_nil_p = assert_eval("class TcFoo; end; TcFoo.new.nil?", false)
  def test_user_obj_class = assert_eval("class TcMyClass; end; TcMyClass.new.class", "TcMyClass")
  def test_method_missing_basic = assert_eval('class TcGhost; def method_missing(n); "got:" + n; end; end; TcGhost.new.anything', "got:anything")
  def test_method_missing_fallback = assert_eval("class TcX; def foo; 1; end; def method_missing(n); 99; end; end; TcX.new.foo", 1)

  # inheritance
  def test_inherit_method = assert_eval("class TcA; def val; 42; end; end; class TcB < TcA; end; TcB.new.val", 42)
  def test_inherit_override = assert_eval("class TcA; def val; 1; end; end; class TcB < TcA; def val; 2; end; end; TcB.new.val", 2)
  def test_inherit_chain = assert_eval("class TcA; def a; 1; end; end; class TcB < TcA; def b; 2; end; end; class TcC < TcB; end; TcC.new.a + TcC.new.b", 3)

  # module / include
  def test_module_include = assert_eval('module TcM; def hello; "hi"; end; end; class TcFoo; include TcM; end; TcFoo.new.hello', "hi")
  def test_module_multi_include = assert_eval(
    'module TcModA; def a; 1; end; end; module TcModB; def b; 2; end; end; ' \
    'class TcFoo; include TcModA; include TcModB; end; TcFoo.new.a + TcFoo.new.b', 3)
  def test_module_class_overrides = assert_eval(
    'module TcM; def val; 1; end; end; class TcFoo; include TcM; def val; 2; end; end; TcFoo.new.val', 2)
  def test_module_inherit_include = assert_eval(
    'module TcM; def val; 42; end; end; class TcA; include TcM; end; class TcB < TcA; end; TcB.new.val', 42)
end
