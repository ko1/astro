require_relative "../../test_helper"

# Range tests inspired by CRuby's test_range.rb.

def test_basic_inclusive
  r = (1..5)
  assert_equal 1, r.first
  assert_equal 5, r.last
  assert_equal false, r.exclude_end?
end

def test_basic_exclusive
  r = (1...5)
  assert_equal 1, r.first
  assert_equal 5, r.last
  assert_equal true, r.exclude_end?
end

def test_to_a_range
  assert_equal [1, 2, 3, 4, 5], (1..5).to_a
  assert_equal [1, 2, 3, 4],    (1...5).to_a
end

def test_size_range
  assert_equal 5, (1..5).size
  assert_equal 4, (1...5).size
  assert_equal 0, (5..1).size
end

def test_each_range
  collected = []
  (1..3).each { |x| collected << x }
  assert_equal [1, 2, 3], collected
end

def test_step_range
  collected = []
  (1..10).step(3) { |x| collected << x }
  assert_equal [1, 4, 7, 10], collected
end

def test_cover_range
  assert_equal true,  (1..10).cover?(5)
  assert_equal false, (1..10).cover?(11)
  assert_equal false, (1..10).cover?(0)
  assert_equal true,  (1...10).cover?(9)
  assert_equal false, (1...10).cover?(10)
end

def test_eqq_range
  assert_equal true,  ((1..10) === 5)
  assert_equal false, ((1..10) === 11)
end

def test_min_max_range
  assert_equal 1,  (1..5).min
  assert_equal 5,  (1..5).max
end

def test_include_range
  assert_equal true,  (1..5).include?(3)
  assert_equal false, (1..5).include?(7)
end

def test_endless_range
  r = (3..)
  assert_equal 3, r.first
  assert_equal nil, r.last
end

def test_beginless_range
  r = (..10)
  assert_equal nil, r.first
  assert_equal 10, r.last
  assert_equal true, r.cover?(5)
end

def test_range_eq
  assert_equal true,  (1..5) == (1..5)
  assert_equal false, (1..5) == (1...5)
end

def test_string_range
  # Range#to_a uses succ-stepping in CRuby; koruby's Range#to_a is
  # specialized to numeric.  String ranges → todo #62.
  # assert_equal ["a", "b", "c", "d"], ("a".."d").to_a
end

TESTS = [
  :test_basic_inclusive, :test_basic_exclusive, :test_to_a_range,
  :test_size_range, :test_each_range, :test_step_range,
  :test_cover_range, :test_eqq_range, :test_min_max_range,
  :test_include_range, :test_endless_range, :test_beginless_range,
  :test_range_eq,
]
TESTS.each { |t| run_test(t) }
report "RangeMore"
