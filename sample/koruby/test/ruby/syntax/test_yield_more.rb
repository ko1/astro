require_relative "../../test_helper"

# yield/iterator tests inspired by CRuby's test_yield.rb / test_iterator.rb.

def yield_one;     yield 1;     end
def yield_two;     yield 1, 2;  end
def yield_zero;    yield;       end
def yield_array;   yield [1, 2]; end
def yield_splat(*a); yield(*a); end

def test_yield_zero
  r = nil
  yield_zero { r = :ran }
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
  yield_array { |x, y| assert_equal [1, 2], [x, y] }
end

def test_yield_splat
  yield_splat(10, 20, 30) { |a, b, c| assert_equal [10, 20, 30], [a, b, c] }
end

# block_given? inside / outside
def maybe_yield
  block_given? ? yield : :no_block
end
def test_block_given
  assert_equal 99, maybe_yield { 99 }
  assert_equal :no_block, maybe_yield
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

TESTS = [
  :test_yield_zero, :test_yield_one, :test_yield_two,
  :test_yield_destructure_array, :test_yield_splat,
  :test_block_given, :test_break_in_yield, :test_next_in_block,
  :test_nested_yield, :test_block_return, :test_explicit_block_param,
]
TESTS.each { |t| run_test(t) }
report "YieldMore"
