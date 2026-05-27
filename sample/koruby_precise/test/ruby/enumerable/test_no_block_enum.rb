require_relative "../../test_helper"

# Enumerator stand-in: each / map / select / each_with_index without
# block returns something that supports `.to_a` and chaining.

def test_array_each_no_block_to_a
  assert_equal [1, 2, 3], [1, 2, 3].each.to_a
end

def test_array_each_no_block_chain
  result = [1, 2, 3].each.map { |x| x * 10 }
  assert_equal [10, 20, 30], result
end

def test_array_each_with_index_no_block
  pairs = [10, 20, 30].each_with_index.to_a
  assert_equal [[10, 0], [20, 1], [30, 2]], pairs
end

def test_array_map_no_block
  result = [1, 2, 3].map.to_a
  assert_equal [1, 2, 3], result
end

def test_range_each_no_block_to_a
  assert_equal [1, 2, 3], (1..3).each.to_a
end

def test_range_map_no_block
  assert_equal [1, 2, 3], (1..3).map.to_a
end

def test_range_each_chain_with_block
  result = (1..3).each.map { |x| x + 100 }
  assert_equal [101, 102, 103], result
end

# ---------- reverse_each ----------

def test_reverse_each_with_block
  out = []
  [1, 2, 3].reverse_each { |x| out << x }
  assert_equal [3, 2, 1], out
end

def test_reverse_each_no_block
  assert_equal [3, 2, 1], [1, 2, 3].reverse_each.to_a
end

TESTS = [
  :test_array_each_no_block_to_a,
  :test_array_each_no_block_chain,
  :test_array_each_with_index_no_block,
  :test_array_map_no_block,
  :test_range_each_no_block_to_a,
  :test_range_map_no_block,
  :test_range_each_chain_with_block,
  :test_reverse_each_with_block,
  :test_reverse_each_no_block,
]
TESTS.each { |t| run_test(t) }
report "NoBlockEnum"
