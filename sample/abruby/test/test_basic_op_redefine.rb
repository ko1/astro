require_relative 'test_helper'

# Verify that basic_op_redefined flag correctly disables type-specialized
# fast paths when built-in operators are redefined.
class TestBasicOpRedefine < AbRubyTest
  # Integer#+
  def test_integer_plus_redefine
    assert_eval <<~RUBY, [3, 42]
      a = 1 + 2
      class Integer
        def +(other); 42; end
      end
      b = 1 + 2
      [a, b]
    RUBY
  end

  # Integer#-
  def test_integer_minus_redefine
    assert_eval <<~RUBY, [1, 99]
      a = 3 - 2
      class Integer
        def -(other); 99; end
      end
      b = 3 - 2
      [a, b]
    RUBY
  end

  # Integer#*
  def test_integer_mul_redefine
    assert_eval <<~RUBY, [6, 0]
      a = 2 * 3
      class Integer
        def *(other); 0; end
      end
      b = 2 * 3
      [a, b]
    RUBY
  end

  # Integer#<
  def test_integer_lt_redefine
    assert_eval <<~RUBY, [true, false]
      a = 1 < 2
      class Integer
        def <(other); false; end
      end
      b = 1 < 2
      [a, b]
    RUBY
  end

  # Integer#==
  def test_integer_eq_redefine
    assert_eval <<~RUBY, [true, false]
      a = 1 == 1
      class Integer
        def ==(other); false; end
      end
      b = 1 == 1
      [a, b]
    RUBY
  end

  # Integer#%
  def test_integer_mod_redefine
    assert_eval <<~RUBY, [1, 77]
      a = 10 % 3
      class Integer
        def %(other); 77; end
      end
      b = 10 % 3
      [a, b]
    RUBY
  end

  # Integer#&
  def test_integer_and_redefine
    assert_eval <<~RUBY, [0, 55]
      a = 5 & 2
      class Integer
        def &(other); 55; end
      end
      b = 5 & 2
      [a, b]
    RUBY
  end

  # Integer#|
  def test_integer_or_redefine
    assert_eval <<~RUBY, [7, 33]
      a = 5 | 2
      class Integer
        def |(other); 33; end
      end
      b = 5 | 2
      [a, b]
    RUBY
  end

  # Redefine does not affect code before the redefinition
  def test_redefine_only_affects_after
    assert_eval <<~RUBY, [100, 42]
      r = 0
      100.times { r = r + 1 }
      a = r
      class Integer
        def +(other); 42; end
      end
      b = 1 + 2
      [a, b]
    RUBY
  end

  # Flag is global: redefining Integer#+ also affects Integer#*
  # (basic_op_redefined disables ALL specializations)
  def test_redefine_disables_all_specializations
    assert_eval <<~RUBY, 42
      class Integer
        def +(other); 42; end
      end
      1 + 2
    RUBY
  end

  # Non-operator method definition does NOT set the flag
  def test_non_operator_no_flag
    assert_eval <<~RUBY, 3
      class Integer
        def foo; 99; end
      end
      1 + 2
    RUBY
  end
end
