require_relative "../../test_helper"

# Integer methods that were missing.

def test_div_truncates_toward_minus_infinity
  assert_equal 3,  7.div(2)
  assert_equal -4, (-7).div(2)        # floor division: -3.5 → -4
  assert_equal -4, 7.div(-2)
  assert_equal 3,  (-7).div(-2)
end

def test_fdiv_returns_float
  assert_equal 3.5, 7.fdiv(2)
  assert_equal 2.5, 5.fdiv(2)
end

def test_fdiv_zero_raises_or_returns_inf
  # CRuby returns Infinity here; we accept either ZeroDivisionError or
  # Float::INFINITY since koruby doesn't have a strict policy yet.
  result = begin
    1.fdiv(0)
  rescue ZeroDivisionError
    :raised
  end
  assert(result == :raised || result == Float::INFINITY,
         "expected raise or Infinity, got #{result.inspect}")
end

def test_size_for_fixnum
  # Fixnum size: 8 bytes on 64-bit (CRuby returns the machine word size).
  assert_equal 8, 1.size
  assert_equal 8, (-1).size
end

TESTS = [
  :test_div_truncates_toward_minus_infinity,
  :test_fdiv_returns_float,
  :test_fdiv_zero_raises_or_returns_inf,
  :test_size_for_fixnum,
]
TESTS.each { |t| run_test(t) }
report "IntegerMore"
