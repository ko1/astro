require_relative "../../test_helper"

# Coverage for Array methods that were missing or buggy in koruby
# before this commit: dig, take_while, drop_while, flat_map (one-level),
# first(n) / last(n), shuffle (any-permutation), and bsearch.

# ---------- Array#dig ----------

def test_dig_basic
  assert_equal 3, [1, [2, [3, 4]]].dig(1, 1, 0)
  assert_equal 4, [1, [2, [3, 4]]].dig(1, 1, 1)
end

def test_dig_returns_nil_on_miss
  assert_equal nil, [1, [2, 3]].dig(5)
  assert_equal nil, [1, [2, 3]].dig(1, 99)
end

def test_dig_into_hash
  assert_equal 9, [{a: 9}].dig(0, :a)
end

def test_dig_no_args_raises
  raised = false
  begin
    [1, 2].dig
  rescue ArgumentError
    raised = true
  end
  assert raised, "Array#dig with no args should raise ArgumentError"
end

# ---------- Array#take_while / drop_while ----------

def test_take_while
  assert_equal [1, 2], [1, 2, 3, 1].take_while { |x| x < 3 }
  assert_equal [],     [3, 1].take_while { |x| x < 3 }
  assert_equal [1, 2, 3, 1], [1, 2, 3, 1].take_while { |_| true }
end

def test_drop_while
  assert_equal [3, 1, 0], [1, 2, 3, 1, 0].drop_while { |x| x < 3 }
  assert_equal [],        [1, 2].drop_while { |_| true }
  assert_equal [1, 2, 3], [1, 2, 3].drop_while { |_| false }
end

# ---------- Array#flat_map ----------

def test_flat_map_one_level
  assert_equal [1, 2, 3, 4],       [[1, 2], [3, 4]].flat_map { |x| x }
  assert_equal [1, -1, 2, -2],     [1, 2].flat_map { |x| [x, -x] }
end

def test_flat_map_non_array_yield
  # If block returns a non-array value, flat_map keeps it as a scalar.
  assert_equal [1, 2, 3], [1, 2, 3].flat_map { |x| x }
end

def test_flat_map_only_flattens_one_level
  assert_equal [[1], [2]], [[[1]], [[2]]].flat_map { |x| x }
end

# ---------- first(n) / last(n) ----------

def test_first_n
  assert_equal [],          [1, 2, 3].first(0)
  assert_equal [1],         [1, 2, 3].first(1)
  assert_equal [1, 2],      [1, 2, 3].first(2)
  assert_equal [1, 2, 3],   [1, 2, 3].first(99)
  assert_equal 1,           [1, 2, 3].first  # zero-arg unchanged
end

def test_last_n
  assert_equal [],          [1, 2, 3].last(0)
  assert_equal [3],         [1, 2, 3].last(1)
  assert_equal [2, 3],      [1, 2, 3].last(2)
  assert_equal [1, 2, 3],   [1, 2, 3].last(99)
  assert_equal 3,           [1, 2, 3].last   # zero-arg unchanged
end

# ---------- shuffle ----------

def test_shuffle_returns_permutation
  src = [1, 2, 3, 4, 5]
  10.times do
    s = src.shuffle
    assert_equal 5, s.length
    assert_equal [1, 2, 3, 4, 5], s.sort
  end
end

def test_shuffle_does_not_mutate
  src = [1, 2, 3]
  src.shuffle
  assert_equal [1, 2, 3], src
end

# ---------- bsearch ----------

def test_bsearch_find_minimum
  a = [0, 4, 7, 10, 12]
  assert_equal 7,   a.bsearch { |x| x >= 6 }
  assert_equal 0,   a.bsearch { |x| x >= 0 }
  assert_equal 12,  a.bsearch { |x| x >= 12 }
  assert_equal nil, a.bsearch { |x| x >= 999 }
end

def test_bsearch_empty
  assert_equal nil, [].bsearch { |_| true }
end

TESTS = [
  :test_dig_basic,
  :test_dig_returns_nil_on_miss,
  :test_dig_into_hash,
  :test_dig_no_args_raises,
  :test_take_while,
  :test_drop_while,
  :test_flat_map_one_level,
  :test_flat_map_non_array_yield,
  :test_flat_map_only_flattens_one_level,
  :test_first_n,
  :test_last_n,
  :test_shuffle_returns_permutation,
  :test_shuffle_does_not_mutate,
  :test_bsearch_find_minimum,
  :test_bsearch_empty,
]
TESTS.each { |t| run_test(t) }
report "ArrayMore"
