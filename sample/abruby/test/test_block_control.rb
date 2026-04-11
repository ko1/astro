require_relative 'test_helper'

# Non-local control flow from inside a block: next, break, return, raise.
# Also covers nesting, interaction with while/rescue/ensure/super, and
# the bit-flag RESULT state invariants.
class TestBlockControl < AbRubyTest
  # === C1. next ===

  def test_next_with_value_used_by_yield
    assert_eval('def f; yield; end; f { next 42 }', 42)
  end

  def test_next_value_participates_in_expression
    assert_eval('def f; (yield) + 1; end; f { next 10 }', 11)
  end

  def test_next_without_value_is_nil
    assert_eval('def f; yield; end; f { next }', nil)
  end

  def test_next_conditional_first_branch
    assert_eval('def f; yield; end; f { next 1 if true; 2 }', 1)
  end

  def test_next_conditional_second_branch
    assert_eval('def f; yield; end; f { next 1 if false; 2 }', 2)
  end

  def test_next_in_if_branch
    assert_eval('
      def f; yield; end
      f {
        if true
          next 11
        else
          22
        end
      }
    ', 11)
  end

  # === C2. break ===

  def test_break_with_value_returns_from_yielding_method
    assert_eval('def f; yield; :never; end; f { break 42 }', 42)
  end

  def test_break_without_value_returns_nil_from_yielder
    assert_eval('def f; yield; :never; end; f { break }', nil)
  end

  def test_break_from_block_skips_code_after_yield
    assert_eval('
      def f
        yield
        "should not reach"
      end
      f { break :ok }
    ', :ok)
  end

  def test_break_conditional_first_branch
    assert_eval('def f; yield; :never; end; f { break 1 if true; 2 }', 1)
  end

  def test_break_conditional_second_branch
    # break not taken → block returns 2, yielding method returns :after
    assert_eval('def f; yield; :after; end; f { break 1 if false; 2 }', :after)
  end

  # === C3. Non-local return from block ===

  def test_return_from_block_through_one_method
    assert_eval('
      def outer
        inner { return 42 }
        :never
      end
      def inner
        yield
        :never_inner
      end
      outer
    ', 42)
  end

  def test_return_from_block_through_two_methods
    assert_eval('
      def a
        b { return 1 }
        :no_a
      end
      def b
        c { yield }
        :no_b
      end
      def c
        yield
        :no_c
      end
      a
    ', 1)
  end

  def test_return_from_block_skips_inner_method_tail
    assert_eval('
      $reached = []
      def outer
        inner { return :bailed }
        $reached << :outer
      end
      def inner
        yield
        $reached << :inner
      end
      r = outer
      [r, $reached]
    ', [:bailed, []])
  end

  def test_return_from_block_reads_outer_local
    assert_eval('
      def f
        x = 1
        g { return x }
      end
      def g; yield; end
      f
    ', 1)
  end

  def test_return_unless_block_given
    assert_eval('
      def f
        return :noblock unless block_given?
        yield
      end
      [f, f { :given }]
    ', [:noblock, :given])
  end

  def test_return_inside_block_conditional
    # Hand-rolled iterator to avoid depending on Array#each (tested in
    # test_block_iterator.rb).  The semantic point here is: `return`
    # inside a block unwinds past the yielding method (`helper`) and
    # returns from the method where the block was defined (`f`).
    assert_eval('
      def helper
        yield 1
        yield 2
        yield 3
      end
      def f
        helper { |x| return x if x == 2 }
        :not_found
      end
      f
    ', 2)
  end

  # === C4. Exceptions in blocks ===

  def test_raise_from_block_caught_outside
    assert_eval('
      def f; yield; :never; end
      begin
        f { raise "boom" }
      rescue => e
        e.message
      end
    ', 'boom')
  end

  def test_raise_from_block_catchable_by_outer_rescue
    assert_eval('
      def f; yield; end
      r = begin
        f { raise }
        :no
      rescue
        :caught
      end
      r
    ', :caught)
  end

  def test_raise_from_block_uncaught_propagates
    assert_eval('
      def f; yield; end
      def g; f { raise "x" }; :never; end
      begin
        g
        :no
      rescue => e
        e.message
      end
    ', 'x')
  end

  # === C5. while + block interactions ===

  def test_while_break_inside_block_body_only_exits_while
    assert_eval('
      def f; yield; end
      f {
        i = 0
        while true
          break if i == 3
          i += 1
        end
        i + 100
      }
    ', 103)
  end

  def test_block_call_inside_while_continues_loop
    assert_eval('
      def f; yield; end
      total = 0
      i = 0
      while i < 3
        f { total += 1 }
        i += 1
      end
      total
    ', 3)
  end

  def test_inner_while_break_does_not_stop_block
    # A block body contains a `while` whose `break` exits only the
    # while — the enclosing block continues on to the next statement
    # and the yielding method resumes afterward.
    assert_eval('
      def f; yield; end
      sum = 0
      i = 0
      while i < 3
        f {
          j = 10
          while true
            break if j == 0
            j -= 1
          end
          sum += 1
        }
        i += 1
      end
      sum
    ', 3)
  end

  # === C6. Nested block control ===

  def test_nested_yield_execution
    assert_eval('
      def f; yield; end
      def g; yield; end
      def h; yield; end
      f { g { h { 1 } } }
    ', 1)
  end

  def test_break_from_inner_block_only_exits_inner_yielder
    assert_eval('
      def g; yield; end
      def f
        g {
          g { break :inner }
          :after_inner
        }
      end
      f
    ', :after_inner)
  end

  def test_next_from_inner_block
    assert_eval('
      def g; yield; end
      def f
        g {
          r = g { next 7 }
          r + 1
        }
      end
      f
    ', 8)
  end

  def test_return_from_nested_block_unwinds_to_outer_method
    assert_eval('
      def g; yield; end
      def outer
        g { g { return :deep } }
        :never
      end
      outer
    ', :deep)
  end

  # === C7. LocalJumpError: yield with no block ===

  def test_yield_without_block_raises
    assert_eval('
      def f; yield; end
      begin
        f
        :no_raise
      rescue => e
        e.message
      end
    ', 'no block given (yield)')
  end

  def test_yield_without_block_caught_inside_method
    assert_eval('
      def f
        begin
          yield
        rescue
          :caught
        end
      end
      f
    ', :caught)
  end

  # === C8. super with block ===

  def test_super_forwards_implicit_block
    assert_eval('
      class A
        def m; yield; end
      end
      class B < A
        def m; super; end
      end
      B.new.m { 42 }
    ', 42)
  end

  def test_super_forwards_block_with_args
    assert_eval('
      class A
        def m(a); yield a; end
      end
      class B < A
        def m(a); super; end
      end
      B.new.m(10) { |x| x + 1 }
    ', 11)
  end

  def test_super_with_explicit_block_replaces_inherited
    assert_eval('
      class A
        def m; yield; end
      end
      class B < A
        def m; super() { 99 }; end
      end
      B.new.m { 1 }
    ', 99)
  end

  # === C9. rescue / ensure ===

  def test_method_rescue_catches_block_raise
    assert_eval('
      def f
        yield
      rescue
        :caught
      end
      f { raise }
    ', :caught)
  end

  def test_ensure_runs_after_break_from_block
    assert_eval('
      $ran = false
      def f
        yield
      ensure
        $ran = true
      end
      f { break :broken }
      $ran
    ', true)
  end

  def test_ensure_runs_after_return_from_block
    assert_eval('
      $ran = false
      def f
        yield
      ensure
        $ran = true
      end
      def outer
        f { return :r }
      end
      r = outer
      [r, $ran]
    ', [:r, true])
  end

  def test_ensure_runs_after_raise_from_block
    assert_eval('
      $ran = false
      def f
        yield
      ensure
        $ran = true
      end
      begin
        f { raise "x" }
      rescue
      end
      $ran
    ', true)
  end

  # === C10. RESULT state bit invariants ===

  def test_raise_from_block_does_not_leak_next
    # If NEXT bit were accidentally set in an error path, `rescue` would
    # not catch it.  Verify the common raise path is clean.
    assert_eval('
      def f; yield; end
      begin
        f { raise "x" }
        :no
      rescue
        :clean
      end
    ', :clean)
  end

  def test_break_from_block_does_not_leak_return
    # If RETURN bit were accidentally set on a `break`, the yielding
    # method would be considered to have returned, not just broken.
    assert_eval('
      def f; yield; :after; end
      f { break :b }
    ', :b)
  end
end
