# Non-local return: `return` inside a block body returns from the
# lexically enclosing method, not from the iterator.

require_relative "../../test_helper"

# --- baseline: return inside the same method ----------------------

def helper_local_return
  10.times { |i| return i if i == 3 }
  -1
end

def test_local_return_inside_block
  assert_equal 3, helper_local_return
end

# --- return propagates past one level of iterator -----------------

def find_first(arr)
  arr.each { |x| return x if x > 10 }
  :not_found
end

def test_return_past_iterator
  assert_equal 42, find_first([1, 5, 42, 99])
  assert_equal :not_found, find_first([1, 2, 3])
end

# --- return from nested blocks ------------------------------------

def first_even_pair(outer, inner)
  outer.each do |a|
    inner.each do |b|
      return [a, b] if (a + b).even?
    end
  end
  nil
end

def test_return_from_nested_blocks
  assert_equal [1, 1], first_even_pair([1, 2], [1, 2])
  assert_equal [2, 2], first_even_pair([2], [1, 2])
end

# --- return inside Enumerable#find equivalent --------------------

class N
  include Enumerable
  def initialize(n); @n = n; end
  def each
    i = 0
    while i < @n
      yield i
      i += 1
    end
  end
end

def test_enumerable_find
  n = N.new(5)
  assert_equal 3, n.find { |x| x > 2 }
  assert_equal nil, n.find { |x| x > 100 }
end

def test_enumerable_include?
  n = N.new(5)
  assert_equal true,  n.include?(3)
  assert_equal false, n.include?(99)
end

def test_enumerable_first
  n = N.new(5)
  assert_equal 0,         n.first
  assert_equal [0, 1, 2], n.first(3)
end

def test_enumerable_any_all_none
  n = N.new(5)
  assert_equal true,  n.any?  { |x| x > 3 }
  assert_equal false, n.any?  { |x| x > 100 }
  assert_equal true,  n.all?  { |x| x >= 0 }
  assert_equal false, n.all?  { |x| x > 0 }
  assert_equal true,  n.none? { |x| x < 0 }
  assert_equal false, n.none? { |x| x == 0 }
end

# --- yield inside block body refers to enclosing method's block ---
# This exercises the outer_block / executing_block mechanism for
# yield (separate from non-local return) — the Enumerable method
# `map` calls `each { |x| arr << yield(x) }`, where the inner `yield`
# must reach the user's block, not recurse into the inner block.

def test_yield_in_nested_block_via_map
  n = N.new(5)
  assert_equal [0, 2, 4, 6, 8], n.map { |x| x * 2 }
end

def test_yield_in_nested_block_via_each_with_index
  n = N.new(3)
  pairs = []
  n.each_with_index { |x, i| pairs << [x, i] }
  assert_equal [[0, 0], [1, 1], [2, 2]], pairs
end

# --- break from block still targets the immediate iterator -------

def test_break_from_block
  result = [1, 2, 3, 4].each { |x| break x * 100 if x == 2 }
  assert_equal 200, result
end

TESTS = [
  :test_local_return_inside_block,
  :test_return_past_iterator,
  :test_return_from_nested_blocks,
  :test_enumerable_find,
  :test_enumerable_include?,
  :test_enumerable_first,
  :test_enumerable_any_all_none,
  :test_yield_in_nested_block_via_map,
  :test_yield_in_nested_block_via_each_with_index,
  :test_break_from_block,
]

TESTS.each { |t| run_test(t) }
report "NonlocalReturn"
