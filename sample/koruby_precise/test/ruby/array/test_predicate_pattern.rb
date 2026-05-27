require_relative "../../test_helper"

# Array#any? / all? / none? / one? with optional pattern argument.
# Pattern is matched via `===`, so Range / Class / Regexp shim work.

def test_any_pattern_class
  assert_equal true,  [1, 2, 3].any?(Integer)
  assert_equal false, ["a", "b"].any?(Integer)
end

def test_all_pattern
  assert_equal true,  [1, 2, 3].all?(Integer)
  assert_equal false, [1, "a", 3].all?(Integer)
end

def test_none_pattern
  assert_equal true,  [1, 2, 3].none?(String)
  assert_equal false, [1, "a"].none?(String)
end

def test_one_pattern
  assert_equal true,  [1, "a", 2].one?(String)
  assert_equal false, [1, "a", "b"].one?(String)
end

def test_any_pattern_range
  assert_equal true,  [1, 2, 3, 10].any?(5..)
  assert_equal false, [1, 2, 3].any?(5..)
end

# Block forms still work.
def test_any_block
  assert_equal true,  [1, 2, 3].any? { |x| x > 2 }
  assert_equal false, [1, 2, 3].any? { |x| x > 99 }
end

def test_one_empty_array
  assert_equal false, [].one?
end

def test_none_empty_array
  assert_equal true, [].none?
end

# Range#cover? on Range argument.
def test_cover_range_inside
  assert_equal true, (1..10).cover?(2..5)
end

def test_cover_range_outside
  assert_equal false, (1..10).cover?(2..15)
end

def test_cover_exclude_end
  # (1..10).cover?(1...11) — inner exclusive, outer inclusive
  assert_equal true, (1..10).cover?(1...11)
end

# Float#to_s shortest round-trip.
def test_float_to_s_short
  assert_equal "3.14", 3.14.to_s
  assert_equal "1.0", 1.0.to_s
  assert_equal "0.5", 0.5.to_s
end

def test_float_inspect_short
  assert_equal "3.14", 3.14.inspect
end

TESTS = [
  :test_any_pattern_class, :test_all_pattern, :test_none_pattern,
  :test_one_pattern, :test_any_pattern_range, :test_any_block,
  :test_one_empty_array, :test_none_empty_array,
  :test_cover_range_inside, :test_cover_range_outside, :test_cover_exclude_end,
  :test_float_to_s_short, :test_float_inspect_short,
]
TESTS.each { |t| run_test(t) }
report "PredicatePattern"
