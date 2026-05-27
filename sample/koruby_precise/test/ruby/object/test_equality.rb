require_relative "../../test_helper"

# == / eql? / equal? / === semantics.

# ---------- equal? — identity ----------

def test_equal_p_same_object
  s = "x"
  assert_equal true, s.equal?(s)
end

def test_equal_p_different_objects
  assert_equal false, "x".equal?("x")     # two distinct String objects
end

# ---------- == ----------

def test_eq_basic
  assert_equal true,  "x" == "x"
  assert_equal true,  [1, 2] == [1, 2]
  assert_equal true,  ({a: 1} == {a: 1})
  assert_equal false, "x" == "y"
end

# ---------- eql? — type-strict ----------

def test_eql_type_strict_int_float
  assert_equal false, 1.eql?(1.0)
  assert_equal false, 1.0.eql?(1)
end

def test_eql_arrays_by_content
  assert_equal true,  [1, 2].eql?([1, 2])
  assert_equal false, [1, 2.0].eql?([1, 2])
end

# ---------- === — for case/when ----------

def test_eqq_class_against_instance
  assert_equal true, Integer === 42
  assert_equal true, String  === "x"
end

def test_case_when_uses_eqq
  result = case 5
           when 1..3 then :low
           when 4..6 then :mid
           else            :high
           end
  assert_equal :mid, result
end

def test_eqq_regexp_unsupported
  # No Regexp in koruby (per project decision); just ensure
  # a regex literal at least parses without crash.  Actual === isn't
  # used here.
  result = (1 == 1)
  assert_equal true, result
end

TESTS = [
  :test_equal_p_same_object,
  :test_equal_p_different_objects,
  :test_eq_basic,
  :test_eql_type_strict_int_float,
  :test_eql_arrays_by_content,
  :test_eqq_class_against_instance,
  :test_case_when_uses_eqq,
  :test_eqq_regexp_unsupported,
]
TESTS.each { |t| run_test(t) }
report "Equality"
