require_relative "../../test_helper"

# Conditional expression forms: ternary, unless, until, modifier-if /
# unless / while, case/when, case/in (partial).

def test_ternary
  assert_equal "yes", (true ? "yes" : "no")
  assert_equal "no",  (false ? "yes" : "no")
  assert_equal "yes", (1 ? "yes" : "no")          # truthy
  assert_equal "no",  (nil ? "yes" : "no")        # nil is falsy
end

def test_unless_basic
  result = unless false
    :ran
  end
  assert_equal :ran, result
end

def test_unless_with_else
  result = unless true
    :a
  else
    :b
  end
  assert_equal :b, result
end

# ---------- modifier forms ----------

def test_modifier_if
  x = 1
  x = 2 if true
  assert_equal 2, x
  x = 3 if false
  assert_equal 2, x
end

def test_modifier_unless
  x = 1
  x = 2 unless false
  assert_equal 2, x
end

def test_modifier_while
  out = []
  out << 1 while out.size < 3
  assert_equal [1, 1, 1], out
end

def test_modifier_until
  i = 0
  i += 1 until i == 3
  assert_equal 3, i
end

# ---------- case/when ----------

def test_case_with_class_match
  result = case [1, 2]
           when Hash    then :hash
           when Array   then :array
           else              :other
           end
  assert_equal :array, result
end

def test_case_with_range
  result = case 7
           when 0..3   then :low
           when 4..6   then :mid
           when 7..9   then :high
           end
  assert_equal :high, result
end

def test_case_no_subject
  x = 5
  result = case
           when x < 3 then :low
           when x < 8 then :mid
           else            :high
           end
  assert_equal :mid, result
end

# ---------- || and && ----------

def test_or_returns_first_truthy
  assert_equal 1,     nil || 1
  assert_equal "a",   "a" || "b"
  # When both are falsy, `||` returns the right operand.
  assert_equal false, nil || false
  assert_equal nil,   false || nil
end

def test_and_returns_last_or_falsy
  assert_equal "b",  "a" && "b"
  assert_equal nil,  nil && "b"
  assert_equal false, false && "b"
end

# ---------- defined? ----------

def test_defined_local
  x = 1
  assert_equal "local-variable", defined?(x)
  assert_equal nil,              defined?(undefined_var)
end

def test_defined_method
  assert_equal "method", defined?(self.to_s)
end

TESTS = [
  :test_ternary,
  :test_unless_basic,
  :test_unless_with_else,
  :test_modifier_if,
  :test_modifier_unless,
  :test_modifier_while,
  :test_modifier_until,
  :test_case_with_class_match,
  :test_case_with_range,
  :test_case_no_subject,
  :test_or_returns_first_truthy,
  :test_and_returns_last_or_falsy,
  :test_defined_local,
  :test_defined_method,
]
TESTS.each { |t| run_test(t) }
report "ConditionalForms"
