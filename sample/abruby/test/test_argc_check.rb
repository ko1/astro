require_relative 'test_helper'

class TestArgcCheck < AbRubyTest
  # === Too many arguments ===

  def test_too_many_args_0_for_1
    err = assert_raises(RuntimeError) { AbRuby.eval('def f; 1; end; f(99)') }
    assert_match(/wrong number of arguments/, err.message)
    assert_match(/given 1, expected 0/, err.message)
  end

  def test_too_many_args_1_for_2
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a); a; end; f(1, 2)') }
    assert_match(/given 2, expected 1/, err.message)
  end

  def test_too_many_args_2_for_3
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a, b); a; end; f(1, 2, 3)') }
    assert_match(/given 3, expected 2/, err.message)
  end

  # === Too few arguments ===

  def test_too_few_args_1_for_0
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a); a; end; f') }
    assert_match(/given 0, expected 1/, err.message)
  end

  def test_too_few_args_2_for_1
    err = assert_raises(RuntimeError) { AbRuby.eval('def f(a, b); a; end; f(1)') }
    assert_match(/given 1, expected 2/, err.message)
  end

  # === Exact match (no error) ===

  def test_exact_0_args
    assert_eval('def f; 42; end; f', 42)
  end

  def test_exact_1_arg
    assert_eval('def f(a); a; end; f(1)', 1)
  end

  def test_exact_2_args
    assert_eval('def f(a, b); a + b; end; f(1, 2)', 3)
  end

  # === Method call with receiver ===

  def test_method_call_too_many
    err = assert_raises(RuntimeError) {
      AbRuby.eval('class C; def f; 1; end; end; C.new.f(99)')
    }
    assert_match(/given 1, expected 0/, err.message)
  end

  def test_method_call_too_few
    err = assert_raises(RuntimeError) {
      AbRuby.eval('class C; def f(a); a; end; end; C.new.f')
    }
    assert_match(/given 0, expected 1/, err.message)
  end

  # === attr_reader (ivar getter) ===

  def test_attr_reader_too_many
    err = assert_raises(RuntimeError) {
      AbRuby.eval('class C; attr_reader :x; def initialize; @x = 1; end; end; C.new.x(99)')
    }
    assert_match(/given 1, expected 0/, err.message)
  end

  def test_attr_reader_ok
    assert_eval('class C; attr_reader :x; def initialize; @x = 42; end; end; C.new.x', 42)
  end

  # === attr_writer (ivar setter) ===

  # attr_writer always takes exactly 1 argument (the assigned value),
  # so there's no way to call it with wrong argc from Ruby syntax.
  # The check is still in place for internal safety.

  def test_attr_writer_ok
    assert_eval('class C; attr_writer :x; attr_reader :x; end; c = C.new; c.x = 5; c.x', 5)
  end

  # === rescue catches ArgumentError ===

  def test_rescue_catches
    assert_eval(
      'def f; 1; end; begin; f(1); rescue; "caught"; end',
      "caught"
    )
  end

  def test_rescue_message
    assert_eval(
      'def f; 1; end; begin; f(1); rescue => e; e.message; end',
      "wrong number of arguments (given 1, expected 0)"
    )
  end

  # === Recursive call with wrong args ===

  def test_recursive_wrong_args
    err = assert_raises(RuntimeError) {
      AbRuby.eval('def f(a); if a > 0; f(a - 1, 99); else; 0; end; end; f(3)')
    }
    assert_match(/given 2, expected 1/, err.message)
  end
end
