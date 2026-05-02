# Tests for Object#tap, then/yield_self, itself.

require_relative "../../test_helper"

def test_tap_returns_self
  result = 42.tap { |x| x * 2 }  # tap discards block return
  assert_equal 42, result
end

def test_tap_yields_self
  captured = nil
  10.tap { |x| captured = x }
  assert_equal 10, captured
end

def test_then_returns_block_value
  assert_equal 84, 42.then { |x| x * 2 }
end

def test_yield_self_alias
  assert_equal 84, 42.yield_self { |x| x * 2 }
end

def test_itself
  assert_equal 7, 7.itself
  assert_equal "hi", "hi".itself
end

def test_tap_chains
  arr = []
  result = 3.tap { |x| arr << x }.tap { |x| arr << x * 10 }
  assert_equal 3, result
  assert_equal [3, 30], arr
end

TESTS = [
  :test_tap_returns_self,
  :test_tap_yields_self,
  :test_then_returns_block_value,
  :test_yield_self_alias,
  :test_itself,
  :test_tap_chains,
]

TESTS.each { |t| run_test(t) }
report "TapThen"
