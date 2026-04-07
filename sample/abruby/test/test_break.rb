require_relative 'test_helper'

class TestBreak < AbRubyTest
  def test_break_basic
    assert_eval('
      i = 0
      while true
        break if i == 5
        i += 1
      end
      i
    ', 5)
  end

  def test_break_with_value
    assert_eval('
      result = while true
        break 42
      end
      result
    ', 42)
  end

  def test_break_no_value_returns_nil
    assert_eval('
      result = while true
        break
      end
      result
    ', nil)
  end

  def test_break_in_nested_while
    # break only exits the innermost while
    assert_eval('
      total = 0
      i = 0
      while i < 3
        j = 0
        while true
          break if j == 2
          j += 1
        end
        total += j
        i += 1
      end
      total
    ', 6)
  end

  def test_break_with_condition
    assert_eval('
      i = 0
      while i < 100
        break if i >= 10
        i += 1
      end
      i
    ', 10)
  end

  def test_break_in_until
    assert_eval('
      i = 0
      until false
        break if i == 3
        i += 1
      end
      i
    ', 3)
  end

  def test_while_normal_returns_nil
    assert_eval('
      result = while false
        1
      end
      result
    ', nil)
  end

  def test_break_does_not_exit_method
    assert_eval('
      def f
        i = 0
        while true
          break if i == 3
          i += 1
        end
        i + 100
      end
      f
    ', 103)
  end

  def test_break_value_from_while
    assert_eval('
      def find_first_over(limit)
        i = 0
        while i < 1000
          return i if i > limit
          i += 1
        end
      end
      find_first_over(5)
    ', 6)
  end
end
