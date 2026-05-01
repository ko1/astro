# Tests for FLONUM (immediate Float) encoding + the Float fast paths
# in EVAL_node_(plus|minus|mul|div|cmp).  Many corner cases:
#   - 0.0 / -0.0 / NaN / Inf — must heap-allocate (out of FLONUM range)
#   - very large / very small / denormal — heap
#   - normal range — immediate
#   - encode/decode round-trip
#   - Float#class still resolves correctly for both forms

require_relative "test_helper"

# --- basic literal handling ---------------------------------------

def test_basic_literals
  assert_equal Float, 0.5.class
  assert_equal Float, 1.0.class
  assert_equal Float, (-1.5).class
end

def test_zero_and_neg_zero
  # 0.0 has all-zero bit pattern → out of FLONUM range → heap.
  # Either way it should equal 0 and have Float class.
  assert_equal Float, 0.0.class
  assert_equal true, 0.0 == 0.0
  assert_equal true, 0.0 == -0.0
  assert_equal true, 0.0 == 0
  assert_equal true, 0 == 0.0
end

def test_inf_and_nan
  # 1.0 / 0 = Infinity (heap — exponent all-1s, can't FLONUM-encode)
  inf = 1.0 / 0
  assert_equal Float, inf.class
  assert_equal true, inf == inf
  # NaN: never equal to anything, including itself
  nan = 0.0 / 0
  assert_equal false, nan == nan
end

# --- normal-range arithmetic (immediate FLONUM) ------------------

def test_arith_simple
  assert_equal 3.0, 1.0 + 2.0
  assert_equal -1.0, 1.0 - 2.0
  assert_equal 6.0, 2.0 * 3.0
  assert_equal 0.5, 1.0 / 2.0
end

def test_arith_negative
  assert_equal -1.5, -0.5 - 1.0
  assert_equal 1.5, 0.5 - -1.0
  assert_equal -3.0, -1.5 * 2.0
end

def test_mixed_int_float
  # Pure-Int hits FIXNUM fast path; mixed should still produce Float.
  assert_equal 2.5, 1 + 1.5
  assert_equal 2.5, 1.5 + 1
  assert_equal 0.5, 1 - 0.5
  assert_equal 0.5, 1.5 - 1
  assert_equal 3.0, 2 * 1.5
  assert_equal 0.5, 1 / 2.0
  assert_equal 2.0, 3.0 / 1.5
end

# --- comparisons across the type boundary -----------------------

def test_compare_flonum_flonum
  assert_equal true, 1.0 < 2.0
  assert_equal false, 2.0 < 1.0
  assert_equal true, 1.0 <= 1.0
  assert_equal true, 2.0 > 1.0
  assert_equal true, 1.0 >= 1.0
end

def test_compare_int_flonum
  assert_equal true, 1 < 1.5
  assert_equal false, 2 < 1.5
  assert_equal true, 1.5 < 2
  assert_equal true, 1 <= 1.0
  assert_equal true, 1.0 <= 1
  assert_equal true, 2 > 1.5
end

# --- round-trip encode/decode (specific bit patterns) ------------

def test_round_trip
  # These all have exponents in the FLONUM-encodable range (top 3 bits
  # of exponent ∈ {011, 100, 101, 110, 111}, but FLONUM allows 011/100).
  vs = [0.5, 1.0, 1.5, 2.0, 100.0, 0.0625, 1234.5678]
  vs.each do |v|
    assert_equal v, v
    assert_equal v, v + 0.0
    assert_equal v, v - 0.0
  end
  # negative variants
  vs.each do |v|
    assert_equal -v, -v
    assert_equal v, -(-v)
  end
end

# --- arrays / hashes of floats ---------------------------------

def test_float_in_array
  a = [1.5, 2.5, 3.5]
  assert_equal 3, a.length
  assert_equal 1.5, a[0]
  assert_equal 3.5, a[2]
  assert_equal true, a == [1.5, 2.5, 3.5]
end

def test_float_as_hash_value
  h = { a: 1.5, b: 2.5 }
  assert_equal 1.5, h[:a]
  assert_equal 2.5, h[:b]
end

# --- Float arithmetic via dispatch (when redef hidden behind here) -

def test_float_dispatched_methods
  # methods that go through dispatch (not the EVAL_node_* fast path).
  # to_i truncates toward zero.
  assert_equal 1, 1.5.to_i
  assert_equal 1, 1.7.to_i
  assert_equal -1, (-1.5).to_i
end

# Run all
TESTS = [
  :test_basic_literals, :test_zero_and_neg_zero, :test_inf_and_nan,
  :test_arith_simple, :test_arith_negative, :test_mixed_int_float,
  :test_compare_flonum_flonum, :test_compare_int_flonum,
  :test_round_trip,
  :test_float_in_array, :test_float_as_hash_value,
  :test_float_dispatched_methods,
]

TESTS.each do |t|
  $current = t
  send(t)
end

if $fail == 0
  puts "OK Flonum (#{$pass})"
else
  puts "FAIL Flonum: #{$fail}/#{$pass + $fail}"
end
