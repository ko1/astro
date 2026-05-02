require_relative "../../test_helper"

# Numeric#coerce, abs2, Float#to_s shows ".0".

# ---------- Float#to_s preserves type ----------

def test_float_to_s_appends_zero
  assert_equal "1.0",  1.0.to_s
  assert_equal "-3.0", (-3.0).to_s
  assert_equal "0.0",  0.0.to_s
end

def test_float_to_s_keeps_decimal
  assert_equal "1.5", 1.5.to_s
  assert_equal "0.25", 0.25.to_s
end

def test_float_to_s_inf_nan
  assert_equal "Infinity", (1.0/0.0).to_s
  assert_equal "NaN",      (0.0/0.0).to_s
end

# ---------- Integer#coerce ----------

def test_int_coerce_with_int
  result = 1.coerce(2)
  assert_equal [2, 1], result
end

def test_int_coerce_with_float
  result = 1.coerce(2.5)
  # CRuby: returns [2.5, 1.0] — both floats
  assert_equal 2.5, result[0]
  assert_equal 1.0, result[1]
end

def test_int_coerce_invalid
  raised = false
  begin
    1.coerce("nope")
  rescue TypeError
    raised = true
  end
  assert raised
end

# ---------- Float#coerce ----------

def test_float_coerce_with_int
  result = 1.5.coerce(2)
  assert_equal 2.0, result[0]
  assert_equal 1.5, result[1]
end

def test_float_coerce_with_float
  result = 1.5.coerce(3.5)
  assert_equal [3.5, 1.5], result
end

# ---------- abs2 ----------

def test_int_abs2
  assert_equal 9,   3.abs2
  assert_equal 9,   (-3).abs2
  assert_equal 0,   0.abs2
end

def test_float_abs2
  result = 2.5.abs2
  assert((result - 6.25).abs < 1e-9, "got #{result}")
end

TESTS = [
  :test_float_to_s_appends_zero,
  :test_float_to_s_keeps_decimal,
  :test_float_to_s_inf_nan,
  :test_int_coerce_with_int,
  :test_int_coerce_with_float,
  :test_int_coerce_invalid,
  :test_float_coerce_with_int,
  :test_float_coerce_with_float,
  :test_int_abs2,
  :test_float_abs2,
]
TESTS.each { |t| run_test(t) }
report "Coerce"
