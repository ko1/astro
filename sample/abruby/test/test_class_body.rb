require_relative 'test_helper'

class TestClassBody < AbRubyTest
  # Kernel methods should be callable from class body.
  def test_kernel_in_class_body = assert_eval(<<~RUBY, 1)
    class A
      X = 1
    end
    A.const_get(:X)
  RUBY

  def test_raise_from_class_body = assert_eval(<<~RUBY, "boom")
    begin
      class A
        raise "boom"
      end
    rescue => e
      e.message
    end
  RUBY

  # visibility no-ops: private/public/protected/module_function accept args.
  def test_private_no_op = assert_eval(<<~RUBY, 1)
    class A
      def foo; 1; end
      private
      def bar; 2; end
      private :bar
    end
    A.new.foo
  RUBY

  def test_public_no_op = assert_eval(<<~RUBY, 3)
    class A
      def foo; 3; end
      public
    end
    A.new.foo
  RUBY

  def test_protected_no_op = assert_eval(<<~RUBY, 4)
    class A
      def foo; 4; end
      protected :foo
    end
    A.new.foo
  RUBY

  def test_module_function_no_op = assert_eval(<<~RUBY, 5)
    module M
      module_function
      def foo; 5; end
    end
    class C
      include M
      def go; foo; end
    end
    C.new.go
  RUBY

  # Constant lookup walks current class's super chain
  def test_const_in_subclass = assert_eval(<<~RUBY, 1)
    class A
      X = 1
    end
    class B < A
      VAL = X
    end
    B.const_get(:VAL)
  RUBY

  def test_const_table_inherited = assert_eval(<<~RUBY, 42)
    class A
      MAPPER = { 0 => 42 }
    end
    class B < A
      V = MAPPER[0]
    end
    B.const_get(:V)
  RUBY

  # Return with multiple values → Array
  def test_return_multi = assert_eval(
    "def f; return 1, 2, 3; end; f", [1, 2, 3])

  def test_return_multi_mixed = assert_eval(
    "def f(x); return x, x + 1; end; f(10)", [10, 11])

  # Keyword hash → trailing Hash arg
  def test_kwhash_as_last_arg = assert_eval(<<~RUBY, 1)
    def f(h); h[:a]; end
    f(a: 1, b: 2)
  RUBY

  def test_kwhash_with_positional = assert_eval(<<~RUBY, 99)
    def f(x, h); x + h[:k]; end
    f(10, k: 89)
  RUBY
end
