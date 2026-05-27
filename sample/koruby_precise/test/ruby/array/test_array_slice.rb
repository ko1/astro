require_relative "../../test_helper"

# Array slicing variants: a[start, len], a[range], a[-i, len], etc.

def test_aref_index_length
  a = [1, 2, 3, 4, 5]
  assert_equal [2, 3, 4], a[1, 3]
  assert_equal [],         a[5, 1]
  assert_equal nil,        a[6, 1]
end

def test_aref_with_range
  a = [1, 2, 3, 4, 5]
  assert_equal [2, 3, 4],   a[1..3]
  assert_equal [2, 3],      a[1...3]
  assert_equal [4, 5],      a[3..]
  assert_equal [1, 2, 3],   a[..2]
end

def test_aref_negative_index
  a = [1, 2, 3, 4, 5]
  assert_equal 5, a[-1]
  assert_equal 4, a[-2]
  assert_equal [4, 5], a[-2, 2]
end

def test_aset_replaces_range
  a = [1, 2, 3, 4, 5]
  a[1..2] = [99]
  assert_equal [1, 99, 4, 5], a
end

def test_flatten_with_depth
  a = [1, [2, [3, [4]]]]
  assert_equal [1, 2, 3, 4],         a.flatten
  assert_equal [1, 2, [3, [4]]],     a.flatten(1)
  assert_equal [1, 2, 3, [4]],       a.flatten(2)
end

# ---------- Array() coercion ----------

def test_array_kernel_coerce
  assert_equal [],         Array(nil)
  assert_equal [1, 2],     Array([1, 2])
  assert_equal [1, 2],     Array(1..2)
end

# ---------- Array.new ----------

def test_array_new_default
  assert_equal [], Array.new
end

def test_array_new_with_default
  assert_equal [0, 0, 0], Array.new(3, 0)
end

def test_array_new_with_block
  assert_equal [0, 1, 4, 9], Array.new(4) { |i| i * i }
end

TESTS = [
  :test_aref_index_length,
  :test_aref_with_range,
  :test_aref_negative_index,
  :test_aset_replaces_range,
  :test_flatten_with_depth,
  :test_array_kernel_coerce,
  :test_array_new_default,
  :test_array_new_with_default,
  :test_array_new_with_block,
]
TESTS.each { |t| run_test(t) }
report "ArraySlice"
