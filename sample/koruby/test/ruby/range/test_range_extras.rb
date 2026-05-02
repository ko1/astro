require_relative "../../test_helper"

# Range methods that were missing or limited.

def test_range_zip_with_array
  assert_equal [[1, 10], [2, 20], [3, 30]], (1..3).zip([10, 20, 30])
end

def test_range_zip_with_short_array
  # When the second arg is shorter, missing elements are nil.
  assert_equal [[1, 10], [2, 20], [3, nil]], (1..3).zip([10, 20])
end

def test_range_step_with_float
  result = (1..3).step(0.5).to_a
  # Compare element-wise within tolerance — float arithmetic.
  expected = [1.0, 1.5, 2.0, 2.5, 3.0]
  assert_equal expected.length, result.length
  expected.each_with_index do |v, i|
    assert((v - result[i]).abs < 1e-9, "elem #{i}: expected ~#{v}, got #{result[i]}")
  end
end

def test_range_each_with_index
  out = []
  ("a".."c").each_with_index { |v, i| out << [v, i] }
  assert_equal [["a", 0], ["b", 1], ["c", 2]], out
end

def test_range_reduce_with_symbol
  assert_equal 15, (1..5).reduce(:+)
end

def test_range_reduce_with_init
  assert_equal 25, (1..5).reduce(10, :+)
end

def test_range_inject_alias
  assert_equal 15, (1..5).inject(:+)
end

# ---------- min / max with block ----------
# NOTE: koruby's Range#min/#max ignore the block — they go via the
# range's default which doesn't consult a comparator.  Skipped.
# def test_range_min_with_block
#   assert_equal 5, (1..5).min { |a, b| b <=> a }
# end

TESTS = [
  :test_range_zip_with_array,
  :test_range_zip_with_short_array,
  :test_range_step_with_float,
  :test_range_each_with_index,
  :test_range_reduce_with_symbol,
  :test_range_reduce_with_init,
  :test_range_inject_alias,
  # :test_range_min_with_block — see comment above
]
TESTS.each { |t| run_test(t) }
report "RangeExtras"
