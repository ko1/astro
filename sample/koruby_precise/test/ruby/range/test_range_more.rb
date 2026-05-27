require_relative "../../test_helper"

# Range methods that were broken or missing in koruby:
#   - first(n) / last(n) overloads (zero-arg form already existed)
#   - step without block returns an Array of stepped values

# ---------- first(n) / last(n) ----------

def test_first_n
  assert_equal 1,           (1..5).first
  assert_equal [],          (1..5).first(0)
  assert_equal [1],         (1..5).first(1)
  assert_equal [1, 2, 3],   (1..5).first(3)
  assert_equal [1, 2, 3, 4, 5], (1..5).first(99)
end

def test_first_n_exclusive
  assert_equal [1, 2, 3], (1...5).first(3)
  assert_equal [1, 2, 3, 4], (1...5).first(99)
end

def test_last_n
  assert_equal 5,           (1..5).last
  assert_equal [],          (1..5).last(0)
  assert_equal [5],         (1..5).last(1)
  assert_equal [3, 4, 5],   (1..5).last(3)
  assert_equal [1, 2, 3, 4, 5], (1..5).last(99)
end

def test_last_n_exclusive
  # Exclusive ranges: last element is end-1.
  assert_equal [3, 4],    (1...5).last(2)
  assert_equal [1, 2, 3, 4], (1...5).last(99)
end

# ---------- step ----------

def test_step_with_block
  collected = []
  (1..10).step(3) { |i| collected << i }
  assert_equal [1, 4, 7, 10], collected
end

def test_step_without_block_returns_array
  # Plain Ruby returns an Enumerator; koruby returns an Array (same
  # semantics for the common `.step(n).to_a` shape).
  assert_equal [1, 4, 7, 10], (1..10).step(3).to_a
  assert_equal [1, 3, 5, 7, 9], (1..10).step(2).to_a
end

def test_step_default_one
  assert_equal [1, 2, 3], (1..3).step.to_a
end

TESTS = [
  :test_first_n,
  :test_first_n_exclusive,
  :test_last_n,
  :test_last_n_exclusive,
  :test_step_with_block,
  :test_step_without_block_returns_array,
  :test_step_default_one,
]
TESTS.each { |t| run_test(t) }
report "RangeMore"
