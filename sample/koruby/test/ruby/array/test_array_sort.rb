require_relative "../../test_helper"

# Array#sort edge cases: empty, single, custom comparator,
# non-Comparable elements, with nil.

def test_sort_empty
  assert_equal [], [].sort
end

def test_sort_single
  assert_equal [42], [42].sort
end

def test_sort_already_sorted
  assert_equal [1, 2, 3], [1, 2, 3].sort
end

def test_sort_reverse
  assert_equal [1, 2, 3], [3, 2, 1].sort
end

def test_sort_with_dup
  assert_equal [1, 1, 2, 3, 3], [3, 1, 2, 1, 3].sort
end

def test_sort_strings
  assert_equal ["a", "b", "c"], ["c", "a", "b"].sort
end

def test_sort_block
  result = [3, 1, 2].sort { |a, b| b <=> a }     # descending
  assert_equal [3, 2, 1], result
end

def test_sort_by_block
  result = [-3, 1, -2].sort_by { |x| x.abs }
  assert_equal [1, -2, -3], result
end

# ---------- destructive sort! ----------

def test_sort_bang_mutates
  a = [3, 1, 2]
  b = a.sort!
  assert_equal [1, 2, 3], a
  assert(a.equal?(b), "sort! should return self")
end

def test_sort_bang_with_block
  a = [3, 1, 2]
  a.sort! { |x, y| y <=> x }
  assert_equal [3, 2, 1], a
end

# ---------- non-destructive sort ----------

def test_sort_does_not_mutate
  a = [3, 1, 2]
  b = a.sort
  assert_equal [3, 1, 2], a
  assert_equal [1, 2, 3], b
end

TESTS = [
  :test_sort_empty, :test_sort_single,
  :test_sort_already_sorted, :test_sort_reverse,
  :test_sort_with_dup, :test_sort_strings,
  :test_sort_block, :test_sort_by_block,
  :test_sort_bang_mutates, :test_sort_bang_with_block,
  :test_sort_does_not_mutate,
]
TESTS.each { |t| run_test(t) }
report "ArraySort"
