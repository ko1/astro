require_relative "../../test_helper"

def test_trailing_splat
  a, *b = [1, 2, 3, 4]
  assert_equal 1, a
  assert_equal [2, 3, 4], b
end

def test_leading_splat
  *a, b = [1, 2, 3, 4]
  assert_equal [1, 2, 3], a
  assert_equal 4, b
end

def test_middle_splat
  a, *b, c = [1, 2, 3, 4]
  assert_equal 1, a
  assert_equal [2, 3], b
  assert_equal 4, c
end

def test_short_array
  # Splat soaks up zero items if array is too short
  a, *b, c = [1, 2]
  assert_equal 1, a
  assert_equal [], b
  assert_equal 2, c
end

def test_multi_pre_post
  a, b, *c, d, e = [1, 2, 3, 4, 5, 6, 7]
  assert_equal 1, a
  assert_equal 2, b
  assert_equal [3, 4, 5], c
  assert_equal 6, d
  assert_equal 7, e
end

def test_splat_no_name
  # `*` with no name discards
  a, *, c = [1, 2, 3, 4]
  assert_equal 1, a
  assert_equal 4, c
end

TESTS = [
  :test_trailing_splat, :test_leading_splat, :test_middle_splat,
  :test_short_array, :test_multi_pre_post, :test_splat_no_name,
]
TESTS.each { |t| run_test(t) }
report "MasgnSplat"
