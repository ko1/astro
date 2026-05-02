require_relative "../../test_helper"

# Additional Array tests culled from CRuby's test_array.rb.

def test_assoc
  a = [[1, "one"], [2, "two"], [3, "three"]]
  assert_equal [2, "two"], a.assoc(2)
  assert_equal nil, a.assoc(99)
end

def test_rassoc
  a = [[1, "one"], [2, "two"], [3, "three"]]
  assert_equal [2, "two"], a.rassoc("two")
  assert_equal nil, a.rassoc("nope")
end

def test_at_negative
  a = [10, 20, 30, 40]
  assert_equal 10, a.at(0)
  assert_equal 40, a.at(-1)
  assert_equal 30, a.at(-2)
  assert_equal nil, a.at(99)
end

def test_clear
  a = [1, 2, 3]
  a.clear
  assert_equal [], a
  assert_equal 0, a.size
end

def test_delete
  a = [1, 2, 3, 2, 1]
  assert_equal 2, a.delete(2)
  assert_equal [1, 3, 1], a
  assert_equal nil, a.delete(99)
end

def test_delete_at
  a = [10, 20, 30, 40]
  assert_equal 20, a.delete_at(1)
  assert_equal [10, 30, 40], a
  assert_equal nil, a.delete_at(99)
end

def test_delete_if
  a = [1, 2, 3, 4, 5]
  a.delete_if { |x| x.even? }
  assert_equal [1, 3, 5], a
end

def test_fill_basic
  a = Array.new(3, 0)
  assert_equal [0, 0, 0], a
  a.fill(7)
  assert_equal [7, 7, 7], a
end

def test_fill_block
  a = Array.new(4) { |i| i * i }
  assert_equal [0, 1, 4, 9], a
end

def test_insert
  a = [1, 2, 3]
  a.insert(1, :a, :b)
  assert_equal [1, :a, :b, 2, 3], a
end

def test_replace
  a = [1, 2, 3]
  a.replace([9, 8, 7])
  assert_equal [9, 8, 7], a
end

def test_rotate
  a = [1, 2, 3, 4, 5]
  assert_equal [3, 4, 5, 1, 2], a.rotate(2)
  assert_equal [5, 1, 2, 3, 4], a.rotate(-1)
end

def test_find_index
  a = [10, 20, 30, 20]
  assert_equal 1, a.find_index(20)
  assert_equal 1, a.find_index { |x| x == 20 }
  assert_equal nil, a.find_index(99)
end

def test_combination_size
  a = [1, 2, 3, 4]
  assert_equal 6, a.combination(2).to_a.size
end

def test_permutation_size
  a = [1, 2, 3]
  assert_equal 6, a.permutation.to_a.size
  assert_equal 6, a.permutation(2).to_a.size
end

def test_product
  assert_equal [[1, 3], [1, 4], [2, 3], [2, 4]], [1, 2].product([3, 4])
end

def test_count_with_block
  a = [1, 2, 3, 4, 5]
  assert_equal 2, a.count { |x| x.even? }
  assert_equal 3, a.count { |x| x.odd? }
end

def test_each_index
  a = [10, 20, 30]
  collected = []
  a.each_index { |i| collected << i }
  assert_equal [0, 1, 2], collected
end

def test_size_alias
  a = [1, 2, 3]
  assert_equal 3, a.length
  assert_equal 3, a.size
end

def test_frozen
  a = [1, 2, 3].freeze
  assert_equal true, a.frozen?
  assert_equal false, [1, 2, 3].frozen?
end

def test_inspect
  assert_equal "[1, 2, 3]", [1, 2, 3].inspect
  assert_equal "[]",        [].inspect
end

def test_flatten_depth
  a = [1, [2, [3, [4]]]]
  assert_equal [1, 2, [3, [4]]], a.flatten(1)
  assert_equal [1, 2, 3, [4]],   a.flatten(2)
  assert_equal [1, 2, 3, 4],     a.flatten
end

def test_dup
  a = [1, 2, 3]
  b = a.dup
  b[0] = 99
  assert_equal [1, 2, 3], a
  assert_equal [99, 2, 3], b
end

def test_clone
  a = [1, 2, 3]
  b = a.clone
  b[0] = 99
  assert_equal [1, 2, 3], a
  assert_equal [99, 2, 3], b
end

def test_eq_eq
  assert_equal true, [1, 2, 3] == [1, 2, 3]
  assert_equal false, [1, 2, 3] == [1, 2]
  assert_equal false, [1, 2, 3] == [1, 2, 4]
end

def test_eql
  assert_equal true,  [1, 2, 3].eql?([1, 2, 3])
  assert_equal false, [1].eql?(1)
end

def test_compare
  assert_equal(-1, [1, 2] <=> [1, 3])
  assert_equal( 1, [1, 3] <=> [1, 2])
  assert_equal( 0, [1, 2] <=> [1, 2])
end

TESTS = [
  :test_assoc, :test_rassoc, :test_at_negative, :test_clear,
  :test_delete, :test_delete_at, :test_delete_if,
  :test_fill_basic, :test_fill_block,
  :test_insert, :test_replace, :test_rotate,
  :test_find_index,
  :test_combination_size, :test_permutation_size, :test_product,
  :test_count_with_block, :test_each_index,
  :test_size_alias, :test_frozen, :test_inspect,
  :test_flatten_depth, :test_dup, :test_clone,
  :test_eq_eq, :test_eql, :test_compare,
]
TESTS.each { |t| run_test(t) }
report "ArrayMore"
