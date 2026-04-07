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

  # attr_reader / attr_writer / attr_accessor
  def test_attr_reader = assert_eval(
    'class TcAR; attr_reader :x; def initialize(v); @x = v; end; end; TcAR.new(42).x', 42)
  def test_attr_writer = assert_eval(
    'class TcAW; attr_writer :x; def x; @x; end; def initialize; @x = 0; end; end; o = TcAW.new; o.x = 99; o.x', 99)
  def test_attr_accessor = assert_eval(
    'class TcAA; attr_accessor :x; def initialize; @x = 0; end; end; o = TcAA.new; o.x = 7; o.x', 7)
  def test_attr_reader_multiple = assert_eval(
    'class TcARM; attr_reader :a, :b; def initialize(x, y); @a = x; @b = y; end; end; o = TcARM.new(3, 4); o.a + o.b', 7)
  def test_attr_accessor_default_nil = assert_eval(
    'class TcADN; attr_accessor :v; end; TcADN.new.v', nil)

  # is_a? / kind_of? / instance_of?
  def test_is_a_same_class = assert_eval(
    'class TcIA; end; TcIA.new.is_a?(TcIA)', true)
  def test_is_a_parent = assert_eval(
    'class TcIA; end; class TcIB < TcIA; end; TcIB.new.is_a?(TcIA)', true)
  def test_is_a_false = assert_eval(
    'class TcIA; end; class TcIB; end; TcIA.new.is_a?(TcIB)', false)
  def test_is_a_object = assert_eval(
    'class TcIA; end; TcIA.new.is_a?(Object)', true)
  def test_kind_of = assert_eval(
    'class TcIA; end; class TcIB < TcIA; end; TcIB.new.kind_of?(TcIA)', true)
  def test_instance_of_exact = assert_eval(
    'class TcIA; end; TcIA.new.instance_of?(TcIA)', true)
  def test_instance_of_parent = assert_eval(
    'class TcIA; end; class TcIB < TcIA; end; TcIB.new.instance_of?(TcIA)', false)
  def test_is_a_integer = assert_eval('5.is_a?(Integer)', true)
  def test_is_a_string = assert_eval('"hello".is_a?(String)', true)

  # super
  def test_super_bare = assert_eval(
    'class TcSA; def val(x); x * 2; end; end; ' \
    'class TcSB < TcSA; def val(x); super + 1; end; end; TcSB.new.val(5)', 11)
  def test_super_no_args = assert_eval(
    'class TcSA; def greet; "hello"; end; end; ' \
    'class TcSB < TcSA; def greet; super; end; end; TcSB.new.greet', "hello")
  def test_super_with_args = assert_eval(
    'class TcSA; def calc(x); x + 10; end; end; ' \
    'class TcSB < TcSA; def calc(x); super(x * 2); end; end; TcSB.new.calc(3)', 16)
  def test_super_empty_parens = assert_eval(
    'class TcSA; def val; 42; end; end; ' \
    'class TcSB < TcSA; def val; super(); end; end; TcSB.new.val', 42)
  def test_super_initialize = assert_eval(
    'class TcSA; def initialize(x); @x = x; end; end; ' \
    'class TcSB < TcSA; def initialize(x); super; @y = x + 1; end; def sum; @x + @y; end; end; ' \
    'TcSB.new(10).sum', 21)
  def test_super_chain = assert_eval(
    'class TcSA; def val; 1; end; end; ' \
    'class TcSB < TcSA; def val; super + 10; end; end; ' \
    'class TcSC < TcSB; def val; super + 100; end; end; TcSC.new.val', 111)

  # eval (note: local variables from outer scope are not visible in eval)
  def test_eval_basic = assert_eval('eval("1 + 2")', 3)
  def test_eval_string = assert_eval('eval("\"hello\"")', "hello")
end
