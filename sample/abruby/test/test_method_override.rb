require_relative 'test_helper'

class TestMethodOverride < AbRubyTest
  # === String interpolation should NOT use String#+ ===

  def test_interpolation_ignores_redefined_plus
    assert_eval('
      class String
        def +(other)
          "WRONG"
        end
      end
      x = "world"
      "hello #{x}"
    ', "hello world")
  end

  # === String interpolation SHOULD use to_s ===

  def test_interpolation_uses_custom_to_s
    assert_eval('
      class Foo
        def to_s = "custom"
      end
      f = Foo.new
      "val=#{f}"
    ', "val=custom")
  end

  # === != should use == ===

  def test_neq_uses_redefined_eq
    assert_eval('
      class Foo
        def ==(other) = true
      end
      a = Foo.new
      b = Foo.new
      a != b
    ', false)
  end

  # === p should use inspect ===

  def test_p_uses_custom_inspect
    # We can't easily capture stdout, but we can verify p returns its argument
    assert_eval('
      class Bar
        def inspect = "BAR"
      end
      b = Bar.new
      p(b)
      b.class
    ', "Bar")
  end

  # === if/while truthiness should NOT use any method ===

  def test_if_truthiness_ignores_redefined_methods
    # Even if we redefine ! on Object, if should still use C-level RTEST
    assert_eval('
      class Object
        def !
          true
        end
      end
      if 1
        "yes"
      else
        "no"
      end
    ', "yes")
  end

  # === Class#new should call initialize ===

  def test_new_calls_initialize
    assert_eval('
      class MyClass
        def initialize(x)
          @x = x * 2
        end
        def x = @x
      end
      MyClass.new(5).x
    ', 10)
  end

  # === Operators are methods (should be overridable) ===

  def test_operator_override
    assert_eval('
      class Vec
        def initialize(x, y)
          @x = x
          @y = y
        end
        def +(other)
          Vec.new(@x + other.x, @y + other.y)
        end
        def x = @x
        def y = @y
        def inspect = "(#{@x},#{@y})"
      end
      a = Vec.new(1, 2)
      b = Vec.new(3, 4)
      c = a + b
      c.x + c.y
    ', 10)
  end

  # === Comparison operators should be overridable ===

  def test_comparison_override
    assert_eval('
      class MyNum
        def initialize(v)
          @v = v
        end
        def <(other)
          @v < other.v
        end
        def v = @v
      end
      a = MyNum.new(3)
      b = MyNum.new(5)
      a < b
    ', true)
  end

  # === method_missing should work with operators ===

  def test_method_missing_operator
    assert_eval('
      class Proxy
        def method_missing(name, x)
          x
        end
      end
      p = Proxy.new
      p + 42
    ', 42)
  end

  # === Inherited method override ===

  def test_inherited_method_override
    assert_eval('
      class A
        def foo = 1
      end
      class B < A
        def foo = 2
      end
      B.new.foo
    ', 2)
  end

  def test_inherited_method_fallback
    assert_eval('
      class A
        def foo = 10
      end
      class B < A
      end
      B.new.foo
    ', 10)
  end

  # === to_s used in various contexts ===

  def test_integer_to_s_in_interpolation
    assert_eval('"num=#{42}"', "num=42")
  end

  def test_nil_to_s_in_interpolation
    assert_eval('"val=#{nil}"', "val=")
  end

  def test_bool_to_s_in_interpolation
    assert_eval('"t=#{true} f=#{false}"', "t=true f=false")
  end
end
