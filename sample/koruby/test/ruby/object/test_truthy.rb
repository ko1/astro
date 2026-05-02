require_relative "../../test_helper"

# Ruby truthiness — only false and nil are falsy.

def test_falsy_only_false_nil
  assert_equal false, !!false
  assert_equal false, !!nil
end

def test_truthy_everything_else
  assert_equal true, !!true
  assert_equal true, !!0
  assert_equal true, !!""
  assert_equal true, !![]
  assert_equal true, !!{}
  assert_equal true, !!:any
  assert_equal true, !!0.0
end

# ---------- if vs ternary on these ----------

def test_if_zero_is_truthy
  result = if 0 then :truthy else :falsy end
  assert_equal :truthy, result
end

def test_if_empty_string_is_truthy
  result = if "" then :truthy else :falsy end
  assert_equal :truthy, result
end

def test_if_empty_array_is_truthy
  result = if [] then :truthy else :falsy end
  assert_equal :truthy, result
end

# ---------- short-circuit ----------

def test_or_short_circuits
  hit_right = false
  result = (true || (hit_right = true && false))
  assert_equal true, result
  assert_equal false, hit_right     # right side never ran
end

def test_and_short_circuits
  hit_right = false
  result = (false && (hit_right = true || true))
  assert_equal false, result
  assert_equal false, hit_right
end

TESTS = [
  :test_falsy_only_false_nil,
  :test_truthy_everything_else,
  :test_if_zero_is_truthy,
  :test_if_empty_string_is_truthy,
  :test_if_empty_array_is_truthy,
  :test_or_short_circuits,
  :test_and_short_circuits,
]
TESTS.each { |t| run_test(t) }
report "Truthy"
