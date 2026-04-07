require_relative 'test_helper'

class TestMultiAssign < AbRubyTest
  def test_basic
    assert_eval('a, b = 1, 2; a', 1)
    assert_eval('a, b = 1, 2; b', 2)
  end

  def test_three_values
    assert_eval('a, b, c = 10, 20, 30; a + b + c', 60)
  end

  def test_fewer_rhs
    # Extra targets get nil
    assert_eval('a, b, c = 1, 2; c', nil)
  end

  def test_more_rhs
    # Extra values are discarded
    assert_eval('a, b = 1, 2, 3; b', 2)
  end

  def test_with_expressions
    assert_eval('a, b = 1 + 2, 3 * 4; a + b', 15)
  end

  def test_string_values
    assert_eval('x, y = "hello", "world"; x + " " + y', "hello world")
  end

  def test_multi_assign_ivar
    assert_eval('
      class Pt
        def initialize(x, y)
          @x, @y = x, y
        end
        def x = @x
        def y = @y
      end
      p = Pt.new(3, 4)
      p.x + p.y
    ', 7)
  end

  def test_multi_assign_gvar
    assert_eval('$a, $b = 10, 20; $a + $b', 30)
  end

  def test_swap
    assert_eval('a = 1; b = 2; a, b = b, a; a', 2)
  end

  # RHS is a method call returning array
  def test_multi_assign_from_call
    assert_eval('
      class TcMA
        def vals; [10, 20, 30]; end
      end
      a, b, c = TcMA.new.vals
      a + b + c
    ', 60)
  end
end
