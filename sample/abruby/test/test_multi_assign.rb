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

  # splat in multi-assign
  def test_splat_same    = assert_eval('ary = [10, 20]; a, b = *ary; a + b', 30)
  def test_splat_fewer   = assert_eval('ary = [1]; a, b = *ary; b', nil)
  def test_splat_more    = assert_eval('ary = [1, 2, 3, 4]; a, b = *ary; a + b', 3)

  # RHS is a method call returning array
  def test_call_same     = assert_eval('class TcMA; def v; [10, 20]; end; end; a, b = TcMA.new.v; a + b', 30)
  def test_call_fewer    = assert_eval('class TcMA; def v; [1]; end; end; a, b = TcMA.new.v; b', nil)
  def test_call_more     = assert_eval('class TcMA; def v; [1, 2, 3, 4]; end; end; a, b = TcMA.new.v; a + b', 3)
end
