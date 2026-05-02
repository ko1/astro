require_relative "../../test_helper"

# Splat / double-splat / forwarding semantics that are easy to get
# subtly wrong.

# ---------- splat in method def ----------

def collect_rest(a, *rest, b)
  [a, rest, b]
end

def test_splat_in_middle_def
  assert_equal [1, [2, 3, 4], 5], collect_rest(1, 2, 3, 4, 5)
  assert_equal [1, [], 2],         collect_rest(1, 2)
end

# ---------- splat in call ----------

def add(a, b, c)
  a + b + c
end

def test_splat_at_call
  args = [1, 2, 3]
  assert_equal 6, add(*args)
end

def test_splat_mixed_at_call
  args = [2, 3]
  assert_equal 6, add(1, *args)
end

# ---------- double splat (kwargs) ----------

def collect_kw(**kw)
  kw
end

def test_double_splat_collects_kwargs
  result = collect_kw(a: 1, b: 2)
  assert_equal({a: 1, b: 2}, result)
end

def test_double_splat_at_call
  h = {x: 1, y: 2}
  result = collect_kw(**h)
  assert_equal({x: 1, y: 2}, result)
end

# ---------- mixed args + splat + block ----------

def call_with(a, *b, c, &blk)
  [a, b, c, blk.call(a)]
end

def test_mixed_args_with_block
  result = call_with(1, 2, 3, 4) { |x| x * 100 }
  assert_equal [1, [2, 3], 4, 100], result
end

# ---------- Array#to_a / to_ary unification ----------

def test_splat_string_does_not_split
  args = "hello"
  result = [*args]
  assert_equal ["hello"], result
end

def test_splat_array_in_literal
  a = [2, 3]
  assert_equal [1, 2, 3, 4], [1, *a, 4]
end

TESTS = [
  :test_splat_in_middle_def,
  :test_splat_at_call,
  :test_splat_mixed_at_call,
  :test_double_splat_collects_kwargs,
  :test_double_splat_at_call,
  :test_mixed_args_with_block,
  :test_splat_string_does_not_split,
  :test_splat_array_in_literal,
]
TESTS.each { |t| run_test(t) }
report "SplatMore"
