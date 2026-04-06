require_relative 'test_helper'

class TestException < AbRubyTest
  # === raise ===

  def test_raise_uncaught
    assert_raises(RuntimeError) { AbRuby.eval('raise "boom"') }
  end

  def test_raise_uncaught_message
    err = assert_raises(RuntimeError) { AbRuby.eval('raise "hello error"') }
    assert_equal "hello error", err.message
  end

  def test_raise_no_args
    assert_raises(RuntimeError) { AbRuby.eval('raise') }
  end

  def test_raise_with_expression
    # raise with non-string expression calls .to_s
    assert_raises(RuntimeError) do
      AbRuby.eval('raise 42')
    end
  end

  # === begin/rescue ===

  def test_rescue_basic
    assert_eval('begin; raise "x"; rescue; 42; end', 42)
  end

  def test_rescue_returns_rescue_value
    assert_eval('begin; raise "x"; rescue; "caught"; end', "caught")
  end

  def test_rescue_no_exception
    assert_eval('begin; 99; rescue; 0; end', 99)
  end

  def test_rescue_with_variable
    assert_eval('begin; raise "hello"; rescue => e; e; end', "hello")
  end

  def test_rescue_variable_is_string
    assert_eval('begin; raise "msg"; rescue => e; e + "!"; end', "msg!")
  end

  def test_rescue_stops_propagation
    # After rescue, execution continues normally
    assert_eval('
      a = 0
      begin
        raise "x"
      rescue
        a = 1
      end
      a
    ', 1)
  end

  def test_rescue_code_after_raise_not_executed
    assert_eval('
      a = 0
      begin
        a = 1
        raise "x"
        a = 2
      rescue
        a
      end
    ', 1)
  end

  # === ensure ===

  def test_ensure_runs_on_normal
    assert_eval('
      a = 0
      begin
        1
      ensure
        a = 99
      end
      a
    ', 99)
  end

  def test_ensure_runs_on_exception
    assert_eval('
      a = 0
      begin
        raise "x"
      rescue
        2
      ensure
        a = 99
      end
      a
    ', 99)
  end

  def test_ensure_value_discarded
    # begin/ensure returns body value, not ensure value
    assert_eval('begin; 42; ensure; 99; end', 42)
  end

  def test_ensure_with_rescue
    assert_eval('
      a = 0
      r = begin
        raise "x"
      rescue => e
        e
      ensure
        a = 1
      end
      a
    ', 1)
  end

  def test_ensure_runs_on_return
    assert_eval('
      a = 0
      def f(a_ary)
        begin
          return 42
        ensure
          a_ary.push(1)
        end
      end
      ary = []
      r = f(ary)
      ary.length
    ', 1)
  end

  # === ensure overrides ===

  def test_ensure_raise_overrides_normal
    assert_raises(RuntimeError) do
      AbRuby.eval('begin; 1; ensure; raise "from ensure"; end')
    end
  end

  def test_ensure_raise_overrides_rescue
    err = assert_raises(RuntimeError) do
      AbRuby.eval('begin; raise "a"; rescue; 1; ensure; raise "b"; end')
    end
    assert_equal "b", err.message
  end

  # === method boundary ===

  def test_raise_across_method
    assert_eval('
      def f
        raise "boom"
      end
      begin
        f
      rescue => e
        e
      end
    ', "boom")
  end

  def test_raise_propagates_through_multiple_methods
    assert_eval('
      def a; raise "deep"; end
      def b; a; end
      def c; b; end
      begin; c; rescue => e; e; end
    ', "deep")
  end

  # === re-raise ===

  def test_reraise_in_rescue
    err = assert_raises(RuntimeError) do
      AbRuby.eval('
        begin
          raise "first"
        rescue
          raise "second"
        end
      ')
    end
    assert_equal "second", err.message
  end

  # === nesting ===

  def test_nested_rescue
    assert_eval('
      begin
        begin
          raise "inner"
        rescue => e
          e + " caught"
        end
      rescue => e2
        "outer: " + e2
      end
    ', "inner caught")
  end

  def test_nested_rescue_propagation
    assert_eval('
      begin
        begin
          raise "x"
        rescue => e
          raise "y"
        end
      rescue => e2
        e2
      end
    ', "y")
  end

  def test_begin_without_rescue_or_ensure
    assert_eval('begin; 42; end', 42)
  end
end
