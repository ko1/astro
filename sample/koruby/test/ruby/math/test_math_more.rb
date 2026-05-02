require_relative "../../test_helper"

# Math module functions.

def test_math_constants
  assert(Math::PI > 3.14 && Math::PI < 3.15, "PI ≈ 3.14")
  assert(Math::E > 2.71 && Math::E < 2.72, "E ≈ 2.72")
end

def test_sin_cos_simple
  assert_equal 0.0, Math.sin(0)
  assert_equal 1.0, Math.cos(0)
  assert((Math.sin(Math::PI / 2) - 1.0).abs < 1e-9, "sin(pi/2) ≈ 1")
end

def test_sqrt
  assert_equal 2.0, Math.sqrt(4)
  assert_equal 5.0, Math.sqrt(25)
  assert_equal 0.0, Math.sqrt(0)
end

def test_log
  assert_equal 0.0, Math.log(1)
  assert((Math.log(Math::E) - 1.0).abs < 1e-9, "log(e) ≈ 1")
end

def test_log2_log10
  assert_equal 3.0, Math.log2(8)
  assert_equal 2.0, Math.log10(100)
end

def test_atan2
  assert_equal 0.0, Math.atan2(0, 1)
  assert((Math.atan2(1, 0) - Math::PI / 2).abs < 1e-9)
end

def test_hypot
  assert_equal 5.0, Math.hypot(3, 4)
end

def test_exp
  assert_equal 1.0, Math.exp(0)
  assert((Math.exp(1) - Math::E).abs < 1e-9)
end

TESTS = [
  :test_math_constants,
  :test_sin_cos_simple,
  :test_sqrt,
  :test_log, :test_log2_log10,
  :test_atan2, :test_hypot, :test_exp,
]
TESTS.each { |t| run_test(t) }
report "MathMore"
