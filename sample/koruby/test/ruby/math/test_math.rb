require_relative "../../test_helper"

def test_math_pi
  assert_equal true, (Math::PI - 3.14159).abs < 0.01
end

def test_math_e
  assert_equal true, (Math::E - 2.71828).abs < 0.01
end

def test_math_sqrt
  assert_equal 4.0, Math.sqrt(16)
  assert_equal 3.0, Math.sqrt(9)
end

def test_math_sin_cos
  assert_equal true, (Math.sin(0) - 0).abs < 1e-9
  assert_equal true, (Math.cos(0) - 1).abs < 1e-9
end

def test_math_log
  assert_equal true, (Math.log(Math::E) - 1).abs < 1e-9
  assert_equal true, (Math.log(100, 10) - 2).abs < 1e-9
end

def test_math_pow_via_exp
  assert_equal true, (Math.exp(1) - Math::E).abs < 1e-9
end

def test_math_atan2
  assert_equal true, (Math.atan2(1, 1) - Math::PI / 4).abs < 1e-9
end

def test_math_hypot
  assert_equal 5.0, Math.hypot(3, 4)
end

TESTS = [
  :test_math_pi,
  :test_math_e,
  :test_math_sqrt,
  :test_math_sin_cos,
  :test_math_log,
  :test_math_pow_via_exp,
  :test_math_atan2,
  :test_math_hypot,
]

TESTS.each { |t| run_test(t) }
report "Math"
