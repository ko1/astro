# Tests for korb_yield fast path (single-arg, single-param) + slow
# path (auto-destructure, varargs, block_given?, yield without block).

require_relative "test_helper"

# --- single-arg, single-param: hot fast path -------------------

def yield1
  yield 42
end

def test_yield_single
  result = nil
  yield1 { |x| result = x }
  assert_equal 42, result
end

# --- single-arg, no block param ---------------------------------

def yield_no_arg
  yield
end

def test_yield_no_arg
  ran = false
  yield_no_arg { ran = true }
  assert_equal true, ran
end

# --- multi-arg, multi-param ------------------------------------

def yield_multi
  yield 1, 2, 3
end

def test_yield_multi
  a, b, c = nil, nil, nil
  yield_multi { |x, y, z| a = x; b = y; c = z }
  assert_equal 1, a
  assert_equal 2, b
  assert_equal 3, c
end

# --- auto-destructure: yield single Array, block has multi params -

def yield_array
  yield [10, 20, 30]
end

def test_yield_destructure
  a, b, c = nil, nil, nil
  yield_array { |x, y, z| a = x; b = y; c = z }
  assert_equal 10, a
  assert_equal 20, b
  assert_equal 30, c
end

# --- block returns value to yielder ----------------------------

def double_via_yield
  yield(5) * 2
end

def test_yield_returns
  result = double_via_yield { |x| x + 1 }
  assert_equal 12, result   # (5+1) * 2
end

# --- yield with no block: raises -------------------------------

def yield_required
  yield
end

def test_yield_no_block_raises
  raised = false
  begin
    yield_required
  rescue
    raised = true
  end
  assert_equal true, raised
end

# --- block_given? ------------------------------------------------

def maybe_yield
  if block_given?
    yield 7
  else
    -1
  end
end

def test_block_given_true
  result = maybe_yield { |x| x * 10 }
  assert_equal 70, result
end

def test_block_given_false
  result = maybe_yield
  assert_equal -1, result
end

# --- nested yield (nested blocks) -------------------------------

def each_two
  yield 1
  yield 2
end

def test_each_two_blocks
  sum = 0
  each_two { |x| sum += x }
  assert_equal 3, sum
end

# --- block reads outer variable (closure) -----------------------

def test_block_closure
  outer = 100
  yield1 { |x| outer = outer + x }
  assert_equal 142, outer  # 100 + 42
end

# --- block uses next ------------------------------------------

def yield_three
  total = 0
  total += yield 1
  total += yield 2
  total += yield 3
  total
end

def test_yield_with_next
  # `next x` inside a block makes yield return x.
  result = yield_three { |x| next x * 10 }
  assert_equal 60, result   # 10 + 20 + 30
end

# Run all
TESTS = [
  :test_yield_single, :test_yield_no_arg, :test_yield_multi,
  :test_yield_destructure, :test_yield_returns,
  :test_yield_no_block_raises,
  :test_block_given_true, :test_block_given_false,
  :test_each_two_blocks, :test_block_closure,
  :test_yield_with_next,
  # NOTE: `break` from a block doesn't yet propagate to the yielding
  # method's caller in koruby — node_while swallows the BREAK state.
  # Add test_block_break once that's fixed.
]

TESTS.each do |t|
  $current = t
  send(t)
end

if $fail == 0
  puts "OK Yield (#{$pass})"
else
  puts "FAIL Yield: #{$fail}/#{$pass + $fail}"
end
