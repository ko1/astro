require_relative "../../test_helper"

# Array.new edge cases — including the shared-object pitfall.

def test_array_new_default_independent
  # `Array.new(3, "x")` shares the SAME string across all slots —
  # mutating one mutates all (this is the classic Ruby pitfall).
  a = Array.new(3, "x")
  assert_equal ["x", "x", "x"], a
  # All 3 slots are the same object identity.
  assert(a[0].equal?(a[1]), "expected shared default object")
end

def test_array_new_block_independent
  # The block form creates a fresh value per index.
  a = Array.new(3) { |i| String.new("x") }
  assert_equal ["x", "x", "x"], a
  # Distinct objects.
  assert(!a[0].equal?(a[1]), "expected distinct default objects")
end

def test_array_new_size
  assert_equal [], Array.new(0)
  assert_equal [nil, nil, nil], Array.new(3)
end

def test_array_new_from_block_indices
  assert_equal [0, 1, 4, 9, 16], Array.new(5) { |i| i * i }
end

# ---------- Array.[] ----------

def test_array_class_aref
  assert_equal [1, 2, 3], Array[1, 2, 3]
end

# ---------- to_a / to_ary ----------

def test_to_a_on_array
  assert_equal [1, 2, 3], [1, 2, 3].to_a
end

TESTS = [
  :test_array_new_default_independent,
  :test_array_new_block_independent,
  :test_array_new_size,
  :test_array_new_from_block_indices,
  :test_array_class_aref,
  :test_to_a_on_array,
]
TESTS.each { |t| run_test(t) }
report "ArrayNewPitfalls"
