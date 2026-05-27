require_relative "../../test_helper"

def test_basic
  a = [1, 2, 3]
  assert_equal 3, a.size
  assert_equal 1, a[0]
  assert_equal 3, a[-1]
  assert_equal nil, a[10]
  assert_equal nil, a[-10]
end

def test_aref_range_or_len
  a = [10, 20, 30, 40, 50]
  assert_equal [20, 30], a[1, 2]
  assert_equal [40, 50], a[3, 5]    # truncates
  assert_equal [], a[5, 1]
  assert_equal [30, 40], a[2..3]
  assert_equal [30], a[2...3]
  assert_equal [40, 50], a[-2, 2]
end

def test_aset_range
  a = [1, 2, 3, 4, 5]
  a[1, 2] = [20, 30, 31]   # replaces 2 elements with 3 — array grows
  assert_equal [1, 20, 30, 31, 4, 5], a
  a = [1, 2, 3, 4, 5]
  a[1, 3] = [9]           # 3 → 1 — shrinks
  assert_equal [1, 9, 5], a
end

def test_push_pop
  a = []
  a << 1
  a << 2
  assert_equal [1, 2], a
  assert_equal 2, a.pop
  assert_equal [1], a
  a.push(3, 4)
  assert_equal [1, 3, 4], a
end

def test_shift_unshift
  a = [1, 2, 3]
  assert_equal 1, a.shift
  assert_equal [2, 3], a
  a.unshift(10, 20)
  assert_equal [10, 20, 2, 3], a
end

def test_each
  s = 0
  [1, 2, 3].each {|x| s += x }
  assert_equal 6, s
end

def test_each_with_index
  pairs = []
  [10, 20, 30].each_with_index {|v, i| pairs << [i, v] }
  assert_equal [[0, 10], [1, 20], [2, 30]], pairs
end

def test_map
  assert_equal [2, 4, 6], [1, 2, 3].map {|x| x * 2 }
  assert_equal [], [].map {|x| x }
end

def test_select_reject
  assert_equal [2, 4], [1, 2, 3, 4, 5].select {|x| x.even? rescue x % 2 == 0 }
  # without even?, manual
  assert_equal [1, 3, 5], [1, 2, 3, 4, 5].select {|x| x % 2 == 1 }
end

def test_reduce
  assert_equal 10, [1, 2, 3, 4].reduce(0) {|a, b| a + b }
  assert_equal 10, [1, 2, 3, 4].inject(0) {|a, b| a + b }
  assert_equal 24, [1, 2, 3, 4].reduce {|a, b| a * b }
end

def test_sort
  assert_equal [1, 2, 3, 4], [3, 1, 4, 2].sort
  assert_equal [1, 2, 3, 4], [4, 3, 2, 1].sort
  assert_equal [], [].sort
end

def test_sort_by
  assert_equal [-1, 2, -3, 4], [-3, -1, 2, 4].sort_by {|x| x.abs }
end

def test_zip
  assert_equal [[1, 4], [2, 5], [3, 6]], [1, 2, 3].zip([4, 5, 6])
end

def test_flatten
  assert_equal [1, 2, 3, 4], [[1, 2], [3, 4]].flatten
  assert_equal [1, 2, 3], [1, [2, [3]]].flatten   # depth 1
end

def test_compact
  assert_equal [1, 2, 3], [1, nil, 2, nil, 3].compact
end

def test_uniq
  assert_equal [1, 2, 3], [1, 2, 1, 3, 2, 1].uniq
end

def test_include
  assert_equal true, [1, 2, 3].include?(2)
  assert_equal false, [1, 2, 3].include?(99)
end

def test_min_max_sum
  a = [5, 1, 3, 9, 2]
  assert_equal 1, a.min
  assert_equal 9, a.max
  assert_equal 20, a.sum
end

def test_min_max_by
  arr = [[1, "z"], [2, "a"], [3, "m"]]
  assert_equal [2, "a"], arr.min_by {|x| x[1] }
  assert_equal [1, "z"], arr.max_by {|x| x[1] }
end

def test_each_slice
  out = []
  [1, 2, 3, 4, 5].each_slice(2) {|s| out << s }
  assert_equal [[1, 2], [3, 4], [5]], out
end

def test_pack_bytes
  s = [78, 69, 83].pack("C*")
  assert_equal 3, s.size
  assert_equal "NES", s
end

def test_concat_minus_plus
  assert_equal [1, 2, 3, 4], [1, 2] + [3, 4]
  assert_equal [1, 3], [1, 2, 3] - [2]
  a = [1, 2]; a.concat([3, 4]); assert_equal [1, 2, 3, 4], a
end

def test_mul
  assert_equal [1, 2, 1, 2, 1, 2], [1, 2] * 3
  assert_equal "1,2,3", [1, 2, 3] * ","
end

def test_index
  assert_equal 1, [10, 20, 30].index(20)
  assert_equal nil, [10, 20, 30].index(99)
end

def test_first_last
  a = [1, 2, 3]
  assert_equal 1, a.first
  assert_equal 3, a.last
end

def test_eq
  assert_equal true, [1, 2, 3] == [1, 2, 3]
  assert_equal false, [1, 2, 3] == [1, 2, 3, 4]
  assert_equal true, [[1, 2], [3]] == [[1, 2], [3]]
end

def test_splat_in_literal
  a = [1, 2, 3]
  b = [0, *a, 4]
  assert_equal [0, 1, 2, 3, 4], b
end

def test_destructure_block
  out = []
  [[1, 'a'], [2, 'b']].each {|n, s| out << [s, n] }
  assert_equal [['a', 1], ['b', 2]], out
end

def test_count
  assert_equal 3, [1, 2, 3].count
  assert_equal 3, [1, 2, 1, 3, 1].count(1)
  assert_equal 1, [1, 2, 3].count(2)
end

def test_drop_take
  a = [1, 2, 3, 4, 5]
  assert_equal [3, 4, 5], a.drop(2)
  assert_equal [1, 2], a.take(2)
end

def test_transpose
  assert_equal [[1, 4], [2, 5], [3, 6]], [[1, 2, 3], [4, 5, 6]].transpose
end

def test_reverse
  assert_equal [3, 2, 1], [1, 2, 3].reverse
  a = [1, 2, 3]; a.reverse!; assert_equal [3, 2, 1], a
end

def test_empty
  assert_equal true, [].empty?
  assert_equal false, [1].empty?
end

# Tests for Array#group_by / partition / each_cons / tally / String#lines.


def test_group_by
  result = [1, 2, 3, 4, 5].group_by { |x| x % 2 }
  assert_equal [1, 3, 5], result[1]
  assert_equal [2, 4], result[0]
end

def test_partition
  evens, odds = [1, 2, 3, 4, 5].partition { |x| x.even? }
  assert_equal [2, 4], evens
  assert_equal [1, 3, 5], odds
end

def test_each_cons
  pairs = []
  [1, 2, 3, 4].each_cons(2) { |a, b| pairs << [a, b] }
  assert_equal [[1, 2], [2, 3], [3, 4]], pairs
end

def test_each_cons_size_3
  triples = []
  [1, 2, 3, 4, 5].each_cons(3) { |a, b, c| triples << [a, b, c] }
  assert_equal [[1, 2, 3], [2, 3, 4], [3, 4, 5]], triples
end

def test_tally
  result = ["a", "b", "a", "c", "b", "a"].tally
  assert_equal 3, result["a"]
  assert_equal 2, result["b"]
  assert_equal 1, result["c"]
end

def test_string_lines
  assert_equal ["foo\n", "bar\n", "baz"], "foo\nbar\nbaz".lines
end

def test_string_lines_no_newline
  assert_equal ["abc"], "abc".lines
end

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

TESTS = %i[
  test_basic test_aref_range_or_len test_aset_range
  test_push_pop test_shift_unshift test_each test_each_with_index
  test_map test_select_reject test_reduce test_sort
  test_sort_by test_zip test_flatten test_compact
  test_uniq test_include test_min_max_sum test_min_max_by
  test_each_slice test_pack_bytes test_concat_minus_plus test_mul
  test_index test_first_last test_eq test_splat_in_literal
  test_destructure_block test_count test_drop_take test_transpose
  test_reverse test_empty test_group_by test_partition
  test_each_cons test_each_cons_size_3 test_tally test_string_lines
  test_string_lines_no_newline test_assoc test_rassoc test_at_negative
  test_clear test_delete test_delete_at test_delete_if
  test_fill_basic test_fill_block test_insert test_replace
  test_rotate test_find_index test_combination_size test_permutation_size
  test_product test_count_with_block test_each_index test_size_alias
  test_frozen test_inspect test_flatten_depth test_dup
  test_clone test_eq_eq test_eql test_compare
]
TESTS.each {|t| run_test(t) }
report "Array"
