require_relative "../../test_helper"

# Endless ranges (Ruby 2.6+ syntax `(1..)`) and #first/#step on them.

def test_endless_first_n
  r = (1..)
  assert_equal [1, 2, 3], r.first(3)
end

def test_endless_first_no_arg
  r = (10..)
  assert_equal 10, r.first
end

def test_endless_first_zero
  assert_equal [], (1..).first(0)
end

def test_endless_with_infinity_end
  r = (1..Float::INFINITY)
  assert_equal [1, 2, 3, 4, 5], r.first(5)
end

def test_endless_lazy_chain
  # `(1..).lazy.map { |x| x * 2 }.first(4)` — endless range with lazy.
  result = (1..).lazy.map { |x| x * 2 }.first(4)
  assert_equal [2, 4, 6, 8], result
end

# ---------- Array enumerator stand-ins (no-block) ----------

def test_each_slice_no_block
  assert_equal [[1, 2], [3, 4], [5]], [1, 2, 3, 4, 5].each_slice(2).to_a
end

def test_each_cons_no_block
  assert_equal [[1, 2], [2, 3], [3, 4]], [1, 2, 3, 4].each_cons(2).to_a
end

TESTS = [
  :test_endless_first_n,
  :test_endless_first_no_arg,
  :test_endless_first_zero,
  :test_endless_with_infinity_end,
  :test_endless_lazy_chain,
  :test_each_slice_no_block,
  :test_each_cons_no_block,
]
TESTS.each { |t| run_test(t) }
report "EndlessRange"
