require_relative "../../test_helper"

# Array Enumerable methods that were missing or buggy:
# one?, each_cons, minmax_by.

# ---------- Array#one? ----------

def test_one_no_block
  assert_equal true,  [1].one?
  assert_equal true,  [false, true, false].one?
  assert_equal false, [1, 2].one?
  assert_equal false, [].one?
end

def test_one_with_block
  assert_equal true,  [1, 2, 3].one? { |x| x > 2 }
  assert_equal false, [1, 2, 3].one? { |x| x > 1 }
  assert_equal false, [1, 2, 3].one? { |x| x > 5 }
end

# ---------- Array#each_cons ----------

def test_each_cons_no_block_returns_array
  assert_equal [[1, 2], [2, 3], [3, 4]], [1, 2, 3, 4].each_cons(2)
  assert_equal [[1, 2, 3], [2, 3, 4]],   [1, 2, 3, 4].each_cons(3)
  assert_equal [],                        [1, 2].each_cons(3)
end

def test_each_cons_with_block
  collected = []
  [1, 2, 3, 4].each_cons(2) { |pair| collected << pair }
  assert_equal [[1, 2], [2, 3], [3, 4]], collected
end

# ---------- Array#minmax_by ----------

def test_minmax_by
  assert_equal [1, -3], [1, -3, 2].minmax_by { |x| x.abs }
  assert_equal [nil, nil], [].minmax_by { |x| x }
end

TESTS = [
  :test_one_no_block,
  :test_one_with_block,
  :test_each_cons_no_block_returns_array,
  :test_each_cons_with_block,
  :test_minmax_by,
]
TESTS.each { |t| run_test(t) }
report "ArrayEnumMore"
