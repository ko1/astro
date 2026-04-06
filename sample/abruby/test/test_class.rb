require_relative 'test_helper'

class TestClass < AbRubyTest
  def test_empty_class = assert_eval("class Foo; end; nil", nil)
  def test_class_def_call = assert_eval("class Foo; def bar; 42; end; end; Foo.new.bar", 42)
  def test_init_no_args = assert_eval("class Foo; def initialize; @x = 99; end; def x; @x; end; end; Foo.new.x", 99)
  def test_init_1_arg = assert_eval("class Foo; def initialize(v); @v = v; end; def v; @v; end; end; Foo.new(42).v", 42)
  def test_init_2_args = assert_eval("class P; def initialize(x, y); @x = x; @y = y; end; def sum; @x + @y; end; end; P.new(3, 4).sum", 7)
  def test_reopen = assert_eval("class Foo; def a; 1; end; end; class Foo; def b; 2; end; end; Foo.new.a + Foo.new.b", 3)
  def test_multiple_instances = assert_eval("class C; def initialize(v); @v = v; end; def v; @v; end; end; a = C.new(10); b = C.new(20); a.v + b.v", 30)
  def test_instance_independence = assert_eval("class C; def initialize(v); @v = v; end; def v; @v; end; end; a = C.new(1); b = C.new(2); c = C.new(3); a.v + b.v + c.v", 6)
  def test_ivar_default_nil = assert_eval("class C; def x; @x; end; end; C.new.x", nil)
  def test_ivar_mutation = assert_eval("class C; def initialize; @x = 0; end; def inc; @x += 1; end; def x; @x; end; end; c = C.new; c.inc; c.inc; c.inc; c.x", 3)
  def test_method_override = assert_eval("class Foo; def val; 1; end; end; a = Foo.new.val; class Foo; def val; 2; end; end; a + Foo.new.val", 3)
  def test_custom_inspect = assert_eval('class Foo; def inspect; "hello"; end; end; Foo.new.inspect', "hello")
  def test_self_method_dispatch = assert_eval("class Calc; def double(x); x * 2; end; def quad(x); self.double(self.double(x)); end; end; Calc.new.quad(3)", 12)
  def test_class_operator = assert_eval("class Vec; def initialize(x); @x = x; end; def +(other); Vec.new(@x + other.x); end; def x; @x; end; end; a = Vec.new(3); b = Vec.new(4); c = a + b; c.x", 7)
  def test_user_obj_eq_identity = assert_eval("class Foo; end; a = Foo.new; a == a", true)
  def test_user_obj_eq_different = assert_eval("class Foo; end; Foo.new == Foo.new", false)
  def test_user_obj_neq = assert_eval("class Foo; end; Foo.new != Foo.new", true)
  def test_user_obj_nil_p = assert_eval("class Foo; end; Foo.new.nil?", false)
  def test_user_obj_class = assert_eval("class MyClass; end; MyClass.new.class", "MyClass")
  def test_method_missing_basic = assert_eval('class Ghost; def method_missing(n); "got:" + n; end; end; Ghost.new.anything', "got:anything")
  def test_method_missing_fallback = assert_eval("class X; def foo; 1; end; def method_missing(n); 99; end; end; X.new.foo", 1)

  # inheritance
  def test_inherit_method = assert_eval("class A; def val; 42; end; end; class B < A; end; B.new.val", 42)
  def test_inherit_override = assert_eval("class A; def val; 1; end; end; class B < A; def val; 2; end; end; B.new.val", 2)
  def test_inherit_chain = assert_eval("class A; def a; 1; end; end; class B < A; def b; 2; end; end; class C < B; end; C.new.a + C.new.b", 3)

  # module / include
  def test_module_include = assert_eval('module M; def hello; "hi"; end; end; class Foo; include M; end; Foo.new.hello', "hi")
  def test_module_multi_include = assert_eval(
    'module A; def a; 1; end; end; module B; def b; 2; end; end; ' \
    'class Foo; include A; include B; end; Foo.new.a + Foo.new.b', 3)
  def test_module_class_overrides = assert_eval(
    'module M; def val; 1; end; end; class Foo; include M; def val; 2; end; end; Foo.new.val', 2)
  def test_module_inherit_include = assert_eval(
    'module M; def val; 42; end; end; class A; include M; end; class B < A; end; B.new.val', 42)
end
