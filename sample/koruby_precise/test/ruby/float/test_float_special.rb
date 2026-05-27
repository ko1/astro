require_relative "../../test_helper"

# Float specials: Infinity, NaN, divmod, comparison.

INF  = 1.0 / 0.0
NINF = -1.0 / 0.0
NAN  = 0.0 / 0.0

def test_infinity
  assert_equal true,  INF.infinite? != nil
  assert_equal 1,     INF.infinite?
  assert_equal -1,    NINF.infinite?
  assert_equal nil,   1.5.infinite?
end

def test_nan
  assert_equal true, NAN.nan?
  assert_equal false, 1.5.nan?
end

def test_finite
  assert_equal true,  1.5.finite?
  assert_equal false, INF.finite?
  assert_equal false, NAN.finite?
end

def test_nan_comparisons_all_false
  assert_equal false, NAN == NAN
  assert_equal false, NAN < 1
  assert_equal false, NAN > 1
  # NaN <=> anything is nil per IEEE-754 / Ruby spec
  assert_equal nil, NAN <=> 1.0
end

def test_inf_arithmetic
  assert_equal INF,  INF + 1
  assert_equal INF,  INF * 2
  assert_equal NINF, -INF
end

# ---------- divmod ----------

def test_float_divmod
  q, r = 7.0.divmod(2)
  assert_equal 3,    q
  assert_equal 1.0,  r
end

def test_float_div_zero_returns_inf_or_nan
  # 1.0 / 0 yields Float::INFINITY without raising in CRuby.
  assert_equal INF, 1.0 / 0
end

# ---------- Float literal forms ----------

def test_float_scientific
  assert_equal 1500.0, 1.5e3
  assert_equal 0.0015, 1.5e-3
end

# ---------- truncate / floor / ceil ----------

def test_float_round_methods
  assert_equal 1, 1.7.floor
  assert_equal 2, 1.2.ceil
  assert_equal 1, 1.4.round
  assert_equal 2, 1.5.round
  assert_equal 1, 1.7.truncate
end

TESTS = [
  :test_infinity,
  :test_nan,
  :test_finite,
  :test_nan_comparisons_all_false,
  :test_inf_arithmetic,
  :test_float_divmod,
  :test_float_div_zero_returns_inf_or_nan,
  :test_float_scientific,
  :test_float_round_methods,
]
TESTS.each { |t| run_test(t) }
report "FloatSpecial"
