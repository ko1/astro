require_relative "../../test_helper"

# Float#round(n) currently returns an Integer for non-zero n
# (it dropped to Integer after rounding regardless of precision).

def test_float_round_default_returns_integer
  assert_equal 1, 1.4.round
  assert_equal 2, 1.5.round
  assert_equal -2, (-1.5).round
end

def test_float_round_with_precision_returns_float
  assert_equal 1.23,  1.2345.round(2)
  assert_equal 1.235, 1.2345.round(3)
  assert_equal 1.0,   1.04.round(1)
end

def test_float_round_negative_precision
  assert_equal 1230.0, 1234.0.round(-1)
  assert_equal 1200.0, 1234.0.round(-2)
  assert_equal 1000.0, 1234.0.round(-3)
end

TESTS = [
  :test_float_round_default_returns_integer,
  :test_float_round_with_precision_returns_float,
  :test_float_round_negative_precision,
]
TESTS.each { |t| run_test(t) }
report "FloatMore"
