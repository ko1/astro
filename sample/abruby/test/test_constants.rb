require_relative 'test_helper'

class TestConstants < AbRubyTest
  def test_const_assign_and_read
    assert_eval('FOO = 42; FOO', 42)
  end

  def test_const_string
    assert_eval('MSG = "hello"; MSG', "hello")
  end

  def test_const_in_class
    assert_eval('
      class MyClass
        VERSION = 10
      end
      MyClass::VERSION
    ', 10)
  end

  def test_const_toplevel
    assert_eval('PI = 3; PI + 1', 4)
  end

  def test_const_overwrite
    assert_eval('X = 1; X = 2; X', 2)
  end

  def test_const_with_expression
    assert_eval('MAX = 10 * 10; MAX', 100)
  end

  # const_get / const_set
  def test_const_get_symbol
    assert_eval('class TcCG; FOO = 99; end; TcCG.const_get(:FOO)', 99)
  end

  def test_const_get_string
    assert_eval('class TcCG; BAR = 7; end; TcCG.const_get("BAR")', 7)
  end

  def test_const_set_symbol
    assert_eval('class TcCS; end; TcCS.const_set(:X, 42); TcCS::X', 42)
  end

  def test_const_set_string
    assert_eval('class TcCS; end; TcCS.const_set("Y", 10); TcCS::Y', 10)
  end
end
