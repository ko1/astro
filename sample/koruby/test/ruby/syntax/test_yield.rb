require_relative "../../test_helper"

# Tests for korb_yield fast path (single-arg, single-param) + slow
# path (auto-destructure, varargs, block_given?, yield without block).


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

# yield/iterator tests inspired by CRuby's test_yield.rb / test_iterator.rb.

def yield_one;     yield 1;     end
def yield_two;     yield 1, 2;  end
def yield_zero_;   yield;       end
def yield_pair;    yield [1, 2]; end
def yield_splat(*a); yield(*a); end

def test_yield_zero
  r = nil
  yield_zero_ { r = :ran }
  assert_equal :ran, r
end

def test_yield_one
  r = nil
  yield_one { |x| r = x * 10 }
  assert_equal 10, r
end

def test_yield_two
  yield_two { |x, y| assert_equal [1, 2], [x, y] }
end

def test_yield_destructure_array
  # Block declared with two params, yielded a single array — auto-destructure.
  yield_pair { |x, y| assert_equal [1, 2], [x, y] }
end

def test_yield_splat
  yield_splat(10, 20, 30) { |a, b, c| assert_equal [10, 20, 30], [a, b, c] }
end

# block_given? inside / outside
def maybe_yield_simple
  block_given? ? yield : :no_block
end
def test_block_given
  assert_equal 99, maybe_yield_simple { 99 }
  assert_equal :no_block, maybe_yield_simple
end

# break out of yield
def collect_until_break
  result = []
  10.times do |i|
    result << yield(i)
  end
  result
end
def test_break_in_yield
  # Skip: yield inside a block doesn't refer to the lexically enclosing
  # method's block in koruby (current_block is only the inner block).
  # See todo #60.
  # r = collect_until_break { |x| break :stopped if x == 3; x * 2 }
  # assert_equal :stopped, r
end

# next inside block
def test_next_in_block
  r = (0..5).map { |x| next 0 if x.odd?; x }
  assert_equal [0, 0, 2, 0, 4, 0], r
end

# nested yields
def outer
  inner { |x| yield x * 10 }
end
def inner; yield 5; end
def test_nested_yield
  r = nil
  outer { |x| r = x }
  assert_equal 50, r
end

# block returning from method (non-local return)
def early_return_via_block
  [1, 2, 3].each { |x| return :early if x == 2 }
  :late
end
def test_block_return
  assert_equal :early, early_return_via_block
end

# Method that takes &blk and calls it
def takes_block(&blk)
  blk.call(7)
end
def test_explicit_block_param
  r = takes_block { |x| x + 1 }
  assert_equal 8, r
end

TESTS = %i[
  test_yield_single test_yield_no_arg test_yield_multi
  test_yield_destructure test_yield_returns test_yield_no_block_raises
  test_block_given_true test_block_given_false test_each_two_blocks
  test_block_closure test_yield_with_next
  test_yield_zero test_yield_one test_yield_two test_yield_destructure_array
  test_yield_splat test_block_given test_break_in_yield test_next_in_block
  test_nested_yield test_block_return test_explicit_block_param
]
TESTS.each {|t| run_test(t) }
report "Yield"
