require_relative "test_helper"

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

TESTS = %i[
  test_basic test_exclude test_each test_map test_select test_reduce
  test_step test_size test_include test_exclude_end test_all_any test_count
  test_splat
]
TESTS.each {|t| run_test(t) }
report("Range")
