require_relative "test_helper"

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

TESTS = %i[
  test_basic test_aref_range_or_len test_aset_range
  test_push_pop test_shift_unshift
  test_each test_each_with_index test_map test_select_reject test_reduce
  test_sort test_sort_by test_zip test_flatten test_compact test_uniq
  test_include test_min_max_sum test_min_max_by test_each_slice
  test_pack_bytes test_concat_minus_plus test_mul test_index test_first_last
  test_eq test_splat_in_literal test_destructure_block
  test_count test_drop_take test_transpose test_reverse test_empty
]
TESTS.each {|t| run_test(t) }
report("Array")
