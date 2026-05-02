require_relative "../../test_helper"

# Various edge cases.

# ---------- multiple-return via array ----------

def returns_pair
  return 1, 2
end

def test_multiple_return_makes_array
  result = returns_pair
  assert_equal [1, 2], result
end

# ---------- chained method calls ----------

def test_chained_calls
  assert_equal 6, [1, 2, 3].map { |x| x * 2 }.first(2).sum
end

# ---------- safe nav operator ----------

def test_safe_nav_returns_nil_on_nil
  v = nil
  assert_equal nil, v&.upcase
end

def test_safe_nav_invokes_on_non_nil
  v = "hello"
  assert_equal "HELLO", v&.upcase
end

# ---------- conditional assignment ----------

def test_or_assign_to_unset
  x = nil
  x ||= 5
  assert_equal 5, x
end

def test_or_assign_keeps_set
  x = 10
  x ||= 5
  assert_equal 10, x
end

def test_and_assign_only_when_set
  x = nil
  x &&= 5
  assert_equal nil, x
  y = 1
  y &&= 5
  assert_equal 5, y
end

# ---------- string concat with numeric coercion ----------

def test_string_concat_with_numeric_via_to_s
  assert_equal "x42", "x" + 42.to_s
end

# ---------- nested heredoc ----------

def test_method_chain_at_end_of_line
  result = [1, 2, 3]
    .map { |x| x + 1 }
    .reverse
  assert_equal [4, 3, 2], result
end

# ---------- range iteration ----------

def test_range_each_value
  out = []
  (1..3).each { |x| out << x * 10 }
  assert_equal [10, 20, 30], out
end

TESTS = [
  :test_multiple_return_makes_array,
  :test_chained_calls,
  :test_safe_nav_returns_nil_on_nil,
  :test_safe_nav_invokes_on_non_nil,
  :test_or_assign_to_unset,
  :test_or_assign_keeps_set,
  :test_and_assign_only_when_set,
  :test_string_concat_with_numeric_via_to_s,
  :test_method_chain_at_end_of_line,
  :test_range_each_value,
]
TESTS.each { |t| run_test(t) }
report "MiscCorners"
