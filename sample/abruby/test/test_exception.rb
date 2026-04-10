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
    # raise with non-string: value is passed as-is (no automatic .to_s)
    assert_eval('begin; raise 42; rescue => e; e.message; end', 42)
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
    assert_eval('begin; raise "hello"; rescue => e; e.message; end', "hello")
  end

  def test_rescue_variable_is_string
    assert_eval('begin; raise "msg"; rescue => e; e.message + "!"; end', "msg!")
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
        e.message
      end
    ', "boom")
  end

  def test_raise_propagates_through_multiple_methods
    assert_eval('
      def a; raise "deep"; end
      def b; a; end
      def c; b; end
      begin; c; rescue => e; e.message; end
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
          e.message + " caught"
        end
      rescue => e2
        "outer: " + e2.message
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
        e2.message
      end
    ', "y")
  end

  def test_begin_without_rescue_or_ensure
    assert_eval('begin; 42; end', 42)
  end

  # === backtrace ===

  def test_backtrace_on_uncaught
    err = assert_raises(RuntimeError) { AbRuby.eval("raise \"boom\"") }
    assert err.backtrace
    assert_operator err.backtrace.size, :>=, 1
  end

  def test_backtrace_method_chain
    code = "def a\nraise \"x\"\nend\ndef b\na\nend\nb"
    err = assert_raises(RuntimeError) { AbRuby.eval(code) }
    bt = err.backtrace
    assert_match(/2:in `a'/, bt[0])        # raise at line 2, containing method a
    assert_match(/5:in `b'/, bt[1])        # a called at line 5, containing method b
    assert_match(/in `<main>'/, bt[2])     # top level
  end

  def test_backtrace_deep_chain
    code = "def a; raise \"deep\"; end\ndef b; a; end\ndef c; b; end\ndef d; c; end\nd"
    err = assert_raises(RuntimeError) { AbRuby.eval(code) }
    bt = err.backtrace
    assert_equal 5, bt.size  # a, b, c, d, <main>
    assert_match(/in `<main>'/, bt.last)
  end

  def test_backtrace_cleared_on_rescue
    # Caught exception's backtrace doesn't leak into next exception
    code = "def f\nbegin\nraise \"a\"\nrescue\nend\nraise \"b\"\nend\nf"
    err = assert_raises(RuntimeError) { AbRuby.eval(code) }
    bt = err.backtrace
    # backtrace is from "raise b" (line 6, inside f), not from "raise a"
    assert_match(/6:in `f'/, bt[0])
    assert_match(/in `<main>'/, bt[1])
  end

  def test_exception_message_method
    assert_eval('begin; raise "hello"; rescue => e; e.message; end', "hello")
  end

  def test_exception_backtrace_method
    code = "begin\nraise \"x\"\nrescue => e\ne.backtrace\nend"
    result = AbRuby.eval(code)
    assert_kind_of Array, result
    assert_operator result.size, :>=, 1
  end

  def test_exception_inspect
    assert_eval('begin; raise "oops"; rescue => e; e.inspect; end', '#<RuntimeError: oops>')
  end

  # === Backtrace line number accuracy (caller_node scheme) ===

  def test_backtrace_raise_line
    # raise location should be the first backtrace entry
    code = "def f\n1\nraise \"x\"\nend\nf"
    err = assert_raises(RuntimeError) { AbRuby.eval(code) }
    assert_match(/3:in `f'/, err.backtrace[0])  # raise on line 3
  end

  def test_backtrace_toplevel_raise
    code = "1\n2\nraise \"x\""
    err = assert_raises(RuntimeError) { AbRuby.eval(code) }
    assert_match(/3:.*<main>/, err.backtrace[0])
  end

  def test_backtrace_division_by_zero
    code = "def a\n1 / 0\nend\ndef b\na\nend\nb"
    err = assert_raises(RuntimeError) { AbRuby.eval(code) }
    bt = err.backtrace
    assert_match(/2:in `a'/, bt[0])   # 1/0 on line 2
    assert_match(/5:in `b'/, bt[1])   # a called on line 5
    assert_match(/in `<main>'/, bt[2])
  end

  def test_backtrace_division_by_zero_toplevel
    code = "1 / 0"
    err = assert_raises(RuntimeError) { AbRuby.eval(code) }
    bt = err.backtrace
    assert_match(/1:.*<main>/, bt[0])
  end

  def test_backtrace_modulo_by_zero
    code = "def f\n1 % 0\nend\nf"
    err = assert_raises(RuntimeError) { AbRuby.eval(code) }
    assert_match(/2:in `f'/, err.backtrace[0])
  end

  def test_backtrace_deep_chain_lines
    code = "def a\nraise \"x\"\nend\ndef b\na\nend\ndef c\nb\nend\ndef d\nc\nend\nd"
    err = assert_raises(RuntimeError) { AbRuby.eval(code) }
    bt = err.backtrace
    assert_equal 5, bt.size
    assert_match(/2:in `a'/, bt[0])    # raise
    assert_match(/5:in `b'/, bt[1])    # a called
    assert_match(/8:in `c'/, bt[2])    # b called
    assert_match(/11:in `d'/, bt[3])   # c called
    assert_match(/13:.*<main>/, bt[4]) # d called
  end

  def test_backtrace_recursive
    code = "def f(n)\nif n < 1\nraise \"x\"\nend\nf(n - 1)\nend\nf(3)"
    err = assert_raises(RuntimeError) { AbRuby.eval(code) }
    bt = err.backtrace
    # f(0) raises on line 3, f(1) calls f on line 5, f(2) calls f on line 5, f(3) calls f on line 5
    assert_match(/3:in `f'/, bt[0])
    assert_match(/5:in `f'/, bt[1])
    assert_match(/5:in `f'/, bt[2])
    assert_match(/5:in `f'/, bt[3])
    assert_match(/7:.*<main>/, bt[4])
    assert_equal 5, bt.size
  end

  def test_backtrace_self_method_call
    # func_call (implicit self) should have correct backtrace
    code = "def inner\nraise \"x\"\nend\ndef outer\ninner\nend\nouter"
    err = assert_raises(RuntimeError) { AbRuby.eval(code) }
    bt = err.backtrace
    assert_match(/2:in `inner'/, bt[0])
    assert_match(/5:in `outer'/, bt[1])
    assert_match(/in `<main>'/, bt[2])
  end

  def test_backtrace_explicit_self_call
    code = "class C\ndef a\nraise \"x\"\nend\ndef b\nself.a\nend\nend\nC.new.b"
    err = assert_raises(RuntimeError) { AbRuby.eval(code) }
    bt = err.backtrace
    assert_match(/3:in `a'/, bt[0])
    assert_match(/6:in `b'/, bt[1])
  end

  def test_backtrace_class_method_call
    code = "class Foo\ndef bar\nraise \"x\"\nend\nend\ndef go\nFoo.new.bar\nend\ngo"
    err = assert_raises(RuntimeError) { AbRuby.eval(code) }
    bt = err.backtrace
    assert_match(/3:in `bar'/, bt[0])
    assert_match(/7:in `go'/, bt[1])
    assert_match(/in `<main>'/, bt[2])
  end

  def test_backtrace_rescue_and_reraise
    code = "def f\nbegin\nraise \"a\"\nrescue => e\nraise \"b\"\nend\nend\nf"
    err = assert_raises(RuntimeError) { AbRuby.eval(code) }
    bt = err.backtrace
    assert_equal "b", err.message
    assert_match(/5:in `f'/, bt[0])   # re-raise on line 5
  end

  def test_backtrace_nested_rescue
    code = "def f\nbegin\nraise \"inner\"\nrescue\nend\nraise \"outer\"\nend\nf"
    err = assert_raises(RuntimeError) { AbRuby.eval(code) }
    assert_equal "outer", err.message
    assert_match(/6:in `f'/, err.backtrace[0])
  end

  def test_backtrace_size_with_require
    # require_relative pushes a <top (required)> frame
    # Just verify backtrace is non-empty and reasonable
    code = "def f; raise \"x\"; end; f"
    err = assert_raises(RuntimeError) { AbRuby.eval(code) }
    assert_operator err.backtrace.size, :>=, 2
  end
end
