require_relative 'test_helper'

# Block-aware built-in iterators: Integer#times, Array#each/map/select/reject,
# Hash#each, Range#each.  Tests exercise the block plumbing end-to-end:
# each builtin cfunc calls abruby_yield internally, and non-local control
# (break/next/return/raise) must propagate correctly through the cfunc.
class TestBlockIterator < AbRubyTest
  # === I1. Integer#times ===

  def test_times_yields_indices
    assert_eval('
      a = []
      3.times { |i| a << i }
      a
    ', [0, 1, 2])
  end

  def test_times_accumulator
    assert_eval('
      sum = 0
      5.times { |i| sum += i }
      sum
    ', 10)
  end

  def test_times_zero_no_yield
    assert_eval('
      called = false
      0.times { called = true }
      called
    ', false)
  end

  def test_times_returns_self
    assert_eval('5.times { }', 5)
  end

  def test_times_break_exits_early
    assert_eval('3.times { break :b }', :b)
  end

  def test_times_break_with_condition
    assert_eval('
      r = 10.times { |i| break i if i == 7 }
      r
    ', 7)
  end

  def test_times_next_skips_iteration
    assert_eval('
      a = []
      5.times { |i| next if i == 2; a << i }
      a
    ', [0, 1, 3, 4])
  end

  def test_times_non_local_return
    assert_eval('
      def f
        10.times { |i| return i if i == 4 }
        :never
      end
      f
    ', 4)
  end

  def test_times_closure_read
    assert_eval('
      base = 100
      sum = 0
      3.times { |i| sum += base + i }
      sum
    ', 303)
  end

  # === I2. Array#each ===

  def test_each_yields_each_element
    assert_eval('
      a = []
      [1, 2, 3].each { |x| a << x * 10 }
      a
    ', [10, 20, 30])
  end

  def test_each_empty_array_no_yield
    assert_eval('
      called = false
      [].each { called = true }
      called
    ', false)
  end

  def test_each_returns_self
    assert_eval('
      r = [1, 2, 3].each { }
      r
    ', [1, 2, 3])
  end

  def test_each_break_with_value
    assert_eval('[1, 2, 3].each { |x| break x if x == 2 }', 2)
  end

  def test_each_next_skips
    assert_eval('
      a = []
      [1, 2, 3, 4].each { |x| next if x == 3; a << x }
      a
    ', [1, 2, 4])
  end

  def test_each_non_local_return
    assert_eval('
      def f
        [1, 2, 3].each { |x| return x if x == 2 }
        :not_found
      end
      f
    ', 2)
  end

  def test_each_mixed_types
    assert_eval('
      out = []
      [1, "a", :s, true].each { |x| out << x }
      out
    ', [1, "a", :s, true])
  end

  def test_each_raise_propagates
    assert_eval('
      begin
        [1, 2, 3].each { raise "boom" }
      rescue => e
        e.message
      end
    ', 'boom')
  end

  # === I3. Array#map ===

  def test_map_doubles
    assert_eval('[1, 2, 3].map { |x| x * 2 }', [2, 4, 6])
  end

  def test_map_empty_array
    assert_eval('[].map { |x| x }', [])
  end

  def test_map_break_abandons_result
    assert_eval('[1, 2, 3].map { |x| break :abort if x == 2; x }', :abort)
  end

  def test_map_next_value_becomes_element
    assert_eval('[1, 2, 3].map { |x| next 0 if x == 2; x }', [1, 0, 3])
  end

  def test_map_inspect_strings
    assert_eval('[1, "a", :b].map { |x| x.inspect }', ['1', '"a"', ':b'])
  end

  def test_map_chained_with_each
    assert_eval('
      collected = []
      [1, 2, 3].map { |x| x * x }.each { |y| collected << y }
      collected
    ', [1, 4, 9])
  end

  # === I4. Array#select ===

  def test_select_filters_by_predicate
    assert_eval('[1, 2, 3, 4].select { |x| x > 2 }', [3, 4])
  end

  def test_select_empty
    assert_eval('[].select { true }', [])
  end

  def test_select_all_false
    assert_eval('[1, 2].select { false }', [])
  end

  def test_select_all_true
    assert_eval('[1, 2].select { true }', [1, 2])
  end

  def test_select_composition
    # select(p) + reject(p) should cover the original array
    assert_eval('
      a = [1, 2, 3, 4, 5]
      s = a.select { |x| x > 2 }
      r = a.reject { |x| x > 2 }
      s + r
    ', [3, 4, 5, 1, 2])
  end

  # === I5. Array#reject ===

  def test_reject_keeps_falsy
    assert_eval('[1, 2, 3, 4].reject { |x| x > 2 }', [1, 2])
  end

  def test_reject_empty
    assert_eval('[].reject { true }', [])
  end

  def test_reject_all_true
    assert_eval('[1, 2].reject { true }', [])
  end

  def test_reject_all_false
    assert_eval('[1, 2].reject { false }', [1, 2])
  end

  # === I6. Hash#each ===

  def test_hash_each_yields_pairs
    assert_eval('
      out = []
      {a: 1, b: 2}.each { |k, v| out << [k, v] }
      out
    ', [[:a, 1], [:b, 2]])
  end

  def test_hash_each_empty
    assert_eval('
      called = false
      {}.each { called = true }
      called
    ', false)
  end

  def test_hash_each_returns_self
    assert_eval('
      r = {a: 1}.each { }
      r
    ', {a: 1})
  end

  def test_hash_each_break_with_value
    assert_eval('{a: 1, b: 2}.each { break :x }', :x)
  end

  def test_hash_each_collects_keys_only
    assert_eval('
      keys = []
      {a: 1, b: 2, c: 3}.each { |k, v| keys << k }
      keys
    ', [:a, :b, :c])
  end

  # === I7. Range#each ===

  def test_range_each_inclusive
    assert_eval('
      a = []
      (1..5).each { |i| a << i }
      a
    ', [1, 2, 3, 4, 5])
  end

  def test_range_each_exclusive
    assert_eval('
      a = []
      (1...5).each { |i| a << i }
      a
    ', [1, 2, 3, 4])
  end

  def test_range_each_single_element
    assert_eval('
      a = []
      (1..1).each { |i| a << i }
      a
    ', [1])
  end

  def test_range_each_empty
    assert_eval('
      called = false
      (1..0).each { called = true }
      called
    ', false)
  end

  def test_range_each_break_value
    assert_eval('(1..100).each { |i| break i if i > 5 }', 6)
  end

  def test_range_each_sum
    assert_eval('
      sum = 0
      (1..10).each { |i| sum += i }
      sum
    ', 55)
  end

  def test_range_each_non_local_return
    assert_eval('
      def f
        (1..10).each { |i| return i if i > 3 }
      end
      f
    ', 4)
  end

  def test_range_each_returns_self
    assert_eval('
      r = (1..3).each { }
      r
    ', 1..3)
  end

  # === I8. Iterator composition ===

  def test_nested_each
    assert_eval('
      sum = 0
      [[1, 2], [3, 4]].each { |pair|
        pair.each { |x| sum += x }
      }
      sum
    ', 10)
  end

  def test_map_chain_each
    assert_eval('
      out = []
      [1, 2, 3].map { |x| x * 2 }.each { |y| out << y }
      out
    ', [2, 4, 6])
  end

  def test_map_select_chain
    assert_eval('[1, 2, 3, 4].map { |x| x * x }.select { |x| x > 5 }', [9, 16])
  end

  def test_nested_times_accumulates
    assert_eval('
      sum = 0
      3.times { |i|
        4.times { |j|
          sum += 1
        }
      }
      sum
    ', 12)
  end

  # === I9. Closure capture in iterators ===

  def test_iterator_accumulate_outer
    assert_eval('
      sum = 0
      [1, 2, 3].each { |x| sum += x }
      sum
    ', 6)
  end

  def test_iterator_counter
    assert_eval('
      cnt = 0
      [:a, :b, :c].each { cnt += 1 }
      cnt
    ', 3)
  end

  def test_iterator_mutable_state
    assert_eval('
      state = :init
      [1, 2, 3].each { |x| state = x }
      state
    ', 3)
  end

  def test_iterator_captures_method_local
    assert_eval('
      def f
        base = 10
        total = 0
        3.times { |i| total += base + i }
        total
      end
      f
    ', 33)
  end

  def test_iterator_captures_and_modifies_ivar
    assert_eval('
      class C
        def initialize; @sum = 0; end
        def total(arr); arr.each { |x| @sum += x }; @sum; end
      end
      C.new.total([1, 2, 3, 4])
    ', 10)
  end

  # === I10. Block-less iterator calls (Phase 2 sentinel) ===

  def test_times_without_block_raises
    assert_eval('
      begin
        5.times
        :no
      rescue => e
        e.message.include?("no block")
      end
    ', true)
  end

  def test_each_without_block_raises
    assert_eval('
      begin
        [1].each
        :no
      rescue => e
        e.message.include?("no block")
      end
    ', true)
  end
end
