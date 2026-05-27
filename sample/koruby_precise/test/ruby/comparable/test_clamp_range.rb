require_relative "../../test_helper"

# Comparable#clamp with a Range argument and the eql?-is-type-strict
# rule — both were broken.

# ---------- clamp with Range ----------

def test_clamp_inclusive_range
  assert_equal 2, 0.clamp(2..5)
  assert_equal 5, 8.clamp(2..5)
  assert_equal 3, 3.clamp(2..5)
end

def test_clamp_endless_range
  assert_equal 5, 3.clamp(5..)
  assert_equal 100, 100.clamp(5..)
end

def test_clamp_beginless_range
  # Beginless ranges are tricky — we just need an upper-only clamp.
  assert_equal 5, 8.clamp(..5)
  assert_equal 1, 1.clamp(..5)
end

def test_clamp_two_args_still_works
  assert_equal 2, 1.clamp(2, 5)
  assert_equal 5, 8.clamp(2, 5)
end

# ---------- eql? type-strictness ----------
# CRuby: Numeric#eql? is type-strict (1.eql?(1.0) == false).  We were
# returning true from both Integer#eql? and Float#eql?.

def test_int_eql_is_type_strict
  assert_equal true,  1.eql?(1)
  assert_equal false, 1.eql?(1.0)
  assert_equal false, 1.eql?(2)
end

def test_float_eql_is_type_strict
  assert_equal true,  1.0.eql?(1.0)
  assert_equal false, 1.0.eql?(1)
  assert_equal false, 1.0.eql?(2.0)
end

TESTS = [
  :test_clamp_inclusive_range,
  :test_clamp_endless_range,
  :test_clamp_beginless_range,
  :test_clamp_two_args_still_works,
  :test_int_eql_is_type_strict,
  :test_float_eql_is_type_strict,
]
TESTS.each { |t| run_test(t) }
report "ClampRange"
