require_relative "../../test_helper"

# Hash Enumerable-shaped methods that were missing.

def test_hash_each_with_object
  # Multi-arg block with leading destructure (`|(k, v), acc|`) is
  # currently broken in koruby — unrelated bug.  Use indexed access on
  # the pair instead so this test stays focused on each_with_object.
  out = {a: 1, b: 2, c: 3}.each_with_object([]) { |kv, acc| acc << kv[1] if kv[1].odd? }
  assert_equal [1, 3], out
end

def test_hash_take
  h = {a: 1, b: 2, c: 3}
  assert_equal [],                       h.take(0)
  assert_equal [[:a, 1]],                h.take(1)
  assert_equal [[:a, 1], [:b, 2]],       h.take(2)
  assert_equal [[:a, 1], [:b, 2], [:c, 3]], h.take(99)
end

def test_hash_flat_map
  result = {a: 1, b: 2}.flat_map { |k, v| [k, v] }
  assert_equal [:a, 1, :b, 2], result
end

def test_hash_flat_map_scalar_block
  # Block returning a non-Array → kept as scalar (one-level flatten).
  result = {a: 1, b: 2}.flat_map { |k, v| v }
  assert_equal [1, 2], result
end

TESTS = [
  :test_hash_each_with_object,
  :test_hash_take,
  :test_hash_flat_map,
  :test_hash_flat_map_scalar_block,
]
TESTS.each { |t| run_test(t) }
report "HashEnum"
