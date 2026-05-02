require_relative "../../test_helper"

def test_basic
  r = 1..5
  assert_equal 1, r.first
  assert_equal 5, r.last
  assert_equal [1, 2, 3, 4, 5], r.to_a
end

def test_exclude
  r = 1...5
  assert_equal [1, 2, 3, 4], r.to_a
end

def test_each
  s = 0
  (1..5).each {|x| s += x }
  assert_equal 15, s
end

def test_map
  assert_equal [2, 4, 6], (1..3).map {|x| x * 2 }
end

def test_select
  assert_equal [2, 4], (1..5).select {|x| x % 2 == 0 }
end

def test_reduce
  assert_equal 15, (1..5).reduce(0) {|a, b| a + b }
end

def test_step
  arr = []
  (1..10).step(2) {|x| arr << x }
  assert_equal [1, 3, 5, 7, 9], arr
end

def test_size
  assert_equal 5, (1..5).size
  assert_equal 4, (1...5).size
  assert_equal 0, (5..1).size
end

def test_include
  r = 1..10
  assert_equal true, r.include?(5)
  assert_equal true, r.include?(1)
  assert_equal true, r.include?(10)
  assert_equal false, r.include?(11)
end

def test_exclude_end
  r = 1...10
  assert_equal true, r.include?(5)
  assert_equal true, r.include?(9)
  assert_equal false, r.include?(10)
end

def test_all_any
  assert_equal true, (1..5).all? {|x| x > 0 }
  assert_equal false, (1..5).all? {|x| x > 2 }
  assert_equal true, (1..5).any? {|x| x > 4 }
  assert_equal false, (1..5).any? {|x| x > 100 }
end

def test_count
  assert_equal 5, (1..5).count
  assert_equal 0, (5...5).count
end

def test_splat
  assert_equal [1, 2, 3], [*1..3]
  assert_equal [0, 1, 2, 3, 4], [0, *1..3, 4]
end

def test_range_min_max
  assert_equal 1, (1..10).min
  assert_equal 10, (1..10).max
  assert_equal 9, (1...10).max
end

def test_range_sum
  assert_equal 55, (1..10).sum
  assert_equal 45, (1...10).sum
end

def test_range_cover
  assert_equal true, (1..10).cover?(5)
  assert_equal false, (1..10).cover?(11)
  assert_equal true, (1...10).cover?(1)
  assert_equal false, (1...10).cover?(10)
end

def test_range_include
  assert_equal true, (1..10).include?(5)
  assert_equal false, (1..10).include?(11)
end

def test_range_map
  assert_equal [1, 4, 9], (1..3).map { |i| i * i }
end

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
  assert_equal ["a", "b", "c", "d"], ("a".."d").to_a
end

def test_symbol_range
  assert_equal [:a, :b, :c], (:a..:c).to_a
end

def test_endless_range_cover
  r = (5..)
  assert_equal true,  r.cover?(5)
  assert_equal true,  r.cover?(99)
  assert_equal false, r.cover?(4)
end

def test_beginless_range_cover
  r = (..10)
  assert_equal true,  r.cover?(10)
  assert_equal true,  r.cover?(0)
  assert_equal true,  r.cover?(-100)
  assert_equal false, r.cover?(11)
end

def test_beginless_excl_range
  r = (...10)
  assert_equal true,  r.cover?(9)
  assert_equal false, r.cover?(10)
end

def test_endless_first
  r = (3..)
  assert_equal 3, r.first
end

TESTS = %i[
  test_basic test_exclude test_each
  test_map test_select test_reduce test_step
  test_size test_include test_exclude_end test_all_any
  test_count test_splat test_range_min_max test_range_sum
  test_range_cover test_range_include test_range_map test_basic_inclusive
  test_basic_exclusive test_to_a_range test_size_range test_each_range
  test_step_range test_cover_range test_eqq_range test_min_max_range
  test_include_range test_endless_range test_beginless_range test_range_eq
  test_string_range test_symbol_range test_endless_range_cover test_beginless_range_cover
  test_beginless_excl_range test_endless_first
]
TESTS.each {|t| run_test(t) }
report "Range"
