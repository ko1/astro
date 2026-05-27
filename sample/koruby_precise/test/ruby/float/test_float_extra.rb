require_relative "../../test_helper"

def test_float_zero
  assert_equal true, 0.0.zero?
  assert_equal false, 0.1.zero?
  assert_equal true, (-0.0).zero?  # -0.0 is also zero in Ruby
end

def test_float_positive_negative
  assert_equal true, 1.5.positive?
  assert_equal false, (-1.5).positive?
  assert_equal true, (-1.5).negative?
  assert_equal false, 0.0.positive?  # 0 is neither
  assert_equal false, 0.0.negative?
end

def test_float_nan_infinite
  nan = 0.0 / 0.0
  inf = 1.0 / 0.0
  assert_equal true, nan.nan?
  assert_equal false, inf.nan?
  assert_equal nil, 1.5.infinite?
  assert_equal 1, inf.infinite?
  assert_equal -1, (-inf).infinite?
  assert_equal true, 1.5.finite?
  assert_equal false, inf.finite?
end

def test_float_to_i
  assert_equal 1, 1.7.to_i
  assert_equal -1, (-1.7).to_i
end

def test_float_divmod
  q, r = 7.5.divmod(2)
  assert_equal 3, q
  assert_equal 1.5, r
end

TESTS = [
  :test_float_zero,
  :test_float_positive_negative,
  :test_float_nan_infinite,
  :test_float_to_i,
  :test_float_divmod,
]

TESTS.each { |t| run_test(t) }
report "FloatExtra"
