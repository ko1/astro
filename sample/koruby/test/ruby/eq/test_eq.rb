# Tests for == / != corner cases — particularly the fast paths in
# node_eq / node_neq and the basic-op redefinition guard.

require_relative "../../test_helper"

# --- identity (FIXNUM/FLONUM/SYMBOL/nil/true/false) -----------------

def test_eq_identity_fixnum
  assert_equal true, 0 == 0
  assert_equal true, 1 == 1
  assert_equal true, -1 == -1
  assert_equal false, 0 == 1
  assert_equal false, 1 == -1
end

def test_eq_identity_float
  assert_equal true, 0.5 == 0.5
  assert_equal true, 1.0 == 1.0
  assert_equal false, 0.5 == 0.6
  assert_equal false, 1.0 == 2.0
end

def test_eq_nil
  assert_equal true, nil == nil
  assert_equal false, nil == 0
  assert_equal false, 0 == nil
  assert_equal false, nil == false
  assert_equal false, false == nil
  assert_equal false, nil == ""
  assert_equal false, nil == []
end

def test_eq_true_false
  assert_equal true, true == true
  assert_equal true, false == false
  assert_equal false, true == false
  assert_equal false, false == true
  assert_equal false, true == 1
  assert_equal false, false == 0
  assert_equal false, true == nil
end

def test_eq_symbol
  assert_equal true, :foo == :foo
  assert_equal false, :foo == :bar
  assert_equal false, :foo == "foo"
  assert_equal false, "foo" == :foo
end

# --- mixed numeric (the corner case my fast path was wrong about) ---

def test_eq_int_float_mixed
  assert_equal true, 1 == 1.0
  assert_equal true, 1.0 == 1
  assert_equal true, 0 == 0.0
  assert_equal true, 0.0 == 0
  assert_equal true, -1 == -1.0
  assert_equal false, 1 == 1.5
  assert_equal false, 1.5 == 1
  assert_equal false, 2 == 1.0
end

def test_neq_int_float_mixed
  assert_equal false, 1 != 1.0
  assert_equal true,  1 != 1.5
  assert_equal false, 0.0 != 0
end

# --- string equality (heap object) --------------------------------

def test_eq_string
  assert_equal true, "foo" == "foo"
  assert_equal false, "foo" == "bar"
  assert_equal false, "foo" == :foo
  assert_equal false, "foo" == 0
  assert_equal false, "foo" == nil
end

# --- array / hash heap equality -----------------------------------

def test_eq_array
  assert_equal true, [] == []
  assert_equal true, [1, 2, 3] == [1, 2, 3]
  assert_equal false, [1, 2, 3] == [1, 2]
  assert_equal false, [] == nil
  assert_equal false, nil == []
end

def test_eq_hash
  assert_equal true, {} == {}
  assert_equal true, {a: 1} == {a: 1}
  assert_equal false, {a: 1} == {a: 2}
  assert_equal false, {} == nil
end

# --- != negation ---------------------------------------------------

def test_neq
  assert_equal false, 1 != 1
  assert_equal true,  1 != 2
  assert_equal false, nil != nil
  assert_equal true,  nil != 0
  assert_equal false, "x" != "x"
  assert_equal true,  "x" != "y"
end

# Redef is in test_eq_redef.rb (separate process — flips a permanent
# global flag).

# Run all
TESTS = [
  :test_eq_identity_fixnum, :test_eq_identity_float,
  :test_eq_nil, :test_eq_true_false, :test_eq_symbol,
  :test_eq_int_float_mixed, :test_neq_int_float_mixed,
  :test_eq_string, :test_eq_array, :test_eq_hash,
  :test_neq,
]

TESTS.each do |t|
  $current = t
  send(t)
end

if $fail == 0
  puts "OK Eq (#{$pass})"
else
  puts "FAIL Eq: #{$fail}/#{$pass + $fail}"
end
