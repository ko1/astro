require_relative "../../test_helper"

def test_single_paren_destructure
  result = []
  [[1, 2], [3, 4]].each { |(a, b)| result << [a, b] }
  assert_equal [[1, 2], [3, 4]], result
end

def test_paren_with_simple
  result = []
  [[1, [2, 3]], [4, [5, 6]]].each { |a, (b, c)| result << [a, b, c] }
  assert_equal [[1, 2, 3], [4, 5, 6]], result
end

def test_two_parens
  result = []
  [[[1, 2], [3, 4]]].each { |(a, b), (c, d)| result << [a, b, c, d] }
  assert_equal [[1, 2, 3, 4]], result
end

def test_hash_each_works
  pairs = []
  {a: 1, b: 2}.each { |k, v| pairs << [k, v] }
  assert_equal [[:a, 1], [:b, 2]], pairs
end

TESTS = [:test_single_paren_destructure, :test_paren_with_simple, :test_two_parens, :test_hash_each_works]
TESTS.each { |t| run_test(t) }
report "BlockDestructure"
