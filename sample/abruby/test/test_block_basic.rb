require_relative 'test_helper'

# Basic block syntax + yield + closure behavior.
# Non-local control (next/break/return/raise) lives in test_block_control.rb,
# built-in iterators in test_block_iterator.rb.
class TestBlockBasic < AbRubyTest
  # === B1. Syntax variations ===

  def test_brace_block
    assert_eval('def f; yield; end; f { 42 }', 42)
  end

  def test_do_end_block
    assert_eval('def f; yield; end; f do 42 end', 42)
  end

  def test_empty_arg_list_with_block
    assert_eval('def f; yield; end; f() { 42 }', 42)
  end

  def test_method_call_with_block
    assert_eval('
      class Box
        def m; yield; end
      end
      Box.new.m { 7 }
    ', 7)
  end

  def test_method_call_with_paren_and_block
    assert_eval('
      class Box
        def m; yield; end
      end
      Box.new.m() { 7 }
    ', 7)
  end

  def test_method_call_with_do_end_block
    assert_eval('
      class Box
        def m; yield; end
      end
      Box.new.m do
        7
      end
    ', 7)
  end

  def test_empty_block_body_returns_nil
    assert_eval('def f; yield; end; f { }', nil)
  end

  def test_empty_block_body_do_end
    assert_eval('def f; yield; end; f do end', nil)
  end

  def test_empty_block_params_pipes
    assert_eval('def f; yield; end; f { || 42 }', 42)
  end

  def test_call_with_args_and_block
    assert_eval('
      def f(a, b)
        (yield) + a + b
      end
      f(1, 2) { 100 }
    ', 103)
  end

  def test_block_chained_on_returned_value
    assert_eval('
      def f; yield; end
      class A
        def m; yield; end
      end
      A.new.m { f { 99 } }
    ', 99)
  end

  # === B2. yield basics ===

  def test_yield_zero_args
    assert_eval('def f; yield; end; f { 42 }', 42)
  end

  def test_yield_one_arg
    assert_eval('def f; yield 1; end; f { |x| x }', 1)
  end

  def test_yield_two_args
    assert_eval('def f; yield 1, 2; end; f { |a, b| a + b }', 3)
  end

  def test_yield_three_args
    assert_eval('def f; yield 1, 2, 3; end; f { |a, b, c| [a, b, c] }', [1, 2, 3])
  end

  def test_sequential_yields_return_last
    assert_eval('
      def f
        yield 1
        yield 2
      end
      f { |x| x * 10 }
    ', 20)
  end

  def test_yield_as_expression_addition
    assert_eval('def f; (yield) * 2; end; f { 3 }', 6)
  end

  def test_yield_as_argument
    assert_eval('
      def wrap(v); v + 1; end
      def f; wrap(yield); end
      f { 10 }
    ', 11)
  end

  def test_yield_twice_accumulates
    assert_eval('
      def f
        a = yield
        b = yield
        a + b
      end
      n = 0
      f { n += 1; n }
    ', 3)
  end

  # === B3. Parameter count mismatch ===

  def test_too_many_args_drops_extras
    assert_eval('def f; yield 1, 2, 3; end; f { |a| a }', 1)
  end

  def test_too_few_args_pads_with_nil
    assert_eval('def f; yield 1; end; f { |a, b| [a, b] }', [1, nil])
  end

  def test_zero_args_with_named_param
    assert_eval('def f; yield; end; f { |a| a }', nil)
  end

  def test_pipes_empty_vs_no_pipes
    assert_eval('def f; yield; end; [f { || 1 }, f { 2 }]', [1, 2])
  end

  # === B4. Closure (outer method locals) ===

  def test_closure_read_outer_local
    assert_eval('
      def f
        x = 10
        y = yield
        x + y
      end
      f { 5 }
    ', 15)
  end

  def test_closure_write_outer_local
    assert_eval('
      def f
        x = 0
        yielder { x = 42 }
        x
      end
      def yielder; yield; end
      f
    ', 42)
  end

  def test_block_param_shadows_outer_local
    assert_eval('
      def f
        x = 10
        yielder { |x| x = 99 }
        x
      end
      def yielder; yield 1; end
      f
    ', 10)
  end

  def test_closure_multiple_outer_locals
    assert_eval('
      def f
        a = 1
        b = 2
        yielder { a + b }
      end
      def yielder; yield; end
      f
    ', 3)
  end

  def test_closure_read_write_counter
    assert_eval('
      def f
        n = 0
        yielder { n += 1 }
        yielder { n += 1 }
        yielder { n += 1 }
        n
      end
      def yielder; yield; end
      f
    ', 3)
  end

  def test_closure_sees_ivar
    assert_eval('
      class C
        def initialize; @x = 10; end
        def with_x; yield @x; end
      end
      C.new.with_x { |v| v + 5 }
    ', 15)
  end

  # === B5. block_given? ===

  def test_block_given_false_without_block
    assert_eval('def f; block_given?; end; f', false)
  end

  def test_block_given_true_with_block
    assert_eval('def f; block_given?; end; f { }', true)
  end

  def test_block_given_conditional_yield
    assert_eval('
      def f
        if block_given?
          yield
        else
          :no
        end
      end
      [f { :yes }, f]
    ', [:yes, :no])
  end

  def test_block_given_in_instance_method
    assert_eval('
      class A
        def m; block_given?; end
      end
      a = A.new
      [a.m, a.m { }]
    ', [false, true])
  end

  # === B6. Explicit self + block ===

  def test_explicit_self_block
    assert_eval('
      class C
        def m; self.helper { 42 }; end
        def helper; yield; end
      end
      C.new.m
    ', 42)
  end

  # === B7. self binding inside block ===

  def test_self_inside_block_is_caller
    assert_eval('
      class A
        def m; yield; end
      end
      class B
        def run; A.new.m { self.class_name }; end
        def class_name; "B"; end
      end
      B.new.run
    ', 'B')
  end

  def test_self_inside_nested_block
    assert_eval('
      class A
        def m; yield; end
      end
      class B
        def run; A.new.m { A.new.m { tag } }; end
        def tag; :b_tag; end
      end
      B.new.run
    ', :b_tag)
  end

  # === yield return value ===

  def test_yield_returns_block_value
    assert_eval('def f; r = yield; r + 1; end; f { 10 }', 11)
  end

  def test_yield_returns_nested_expression
    assert_eval('
      def f; yield + yield; end
      n = 0
      f { n += 1 }
    ', 3)
  end
end
