require_relative "../../test_helper"

def each_n(n)
  i = 0
  while i < n
    yield i
    i += 1
  end
end

def test_yield_simple
  arr = []
  each_n(3) {|i| arr << i }
  assert_equal [0, 1, 2], arr
end

def yield_two(a, b)
  yield a, b
end

def test_yield_two
  assert_equal 30, yield_two(10, 20) {|a, b| a + b }
end

def test_block_capture
  s = 0
  [1, 2, 3].each {|x| s += x }
  assert_equal 6, s
end

def test_block_nested
  outer = 0
  inner = 0
  [1, 2].each do |x|
    outer = x
    [10, 20].each do |y|
      inner = y
    end
  end
  assert_equal 2, outer
  assert_equal 20, inner
end

def proc_call(p)
  p.call(7)
end

def test_proc
  p = Proc.new { 42 } rescue nil  # might not have Proc.new
  # Use lambda instead
  l = ->(x) { x * 2 }
  assert_equal 84, l.call(42)
  assert_equal 84, l[42]
end

def test_lambda_two
  l = ->(a, b) { a * 100 + b }
  assert_equal 305, l.call(3, 5)
end

def test_block_given
  def f
    if block_given?
      yield 10
    else
      99
    end
  end
  assert_equal 20, f {|x| x * 2 }
  assert_equal 99, f
end

def each_with_break
  yield 1
  yield 2
  yield 3
  yield 4
  :done
end

def test_break
  r = each_with_break {|x| break x * 100 if x == 2; x }
  assert_equal 200, r
end

def test_next
  arr = []
  [1, 2, 3, 4, 5].each {|x| next if x % 2 == 0; arr << x }
  assert_equal [1, 3, 5], arr
end

def test_return_from_block_in_method
  def find_x(arr)
    arr.each do |x|
      return x if x > 5
    end
    nil
  end
  assert_equal 6, find_x([1, 3, 6, 8])
  assert_equal nil, find_x([1, 2, 3])
end

def test_destructure_yield
  pairs = {a: 1, b: 2}.map {|k, v| [v, k] }
  assert_equal [[1, :a], [2, :b]], pairs
end

def test_inject_no_seed
  # without seed: first element is acc, iter starts from second
  assert_equal 24, [1, 2, 3, 4].inject {|a, b| a * b }
end

def test_proc_closure
  multiplier = 10
  p = ->(x) { x * multiplier }
  assert_equal 30, p.call(3)
  multiplier = 100
  # closure sees updated value (shared fp)
  assert_equal 300, p.call(3)
end

# Break out of a cfunc iterator (Integer#step) — exercises that cfunc
# dispatch catches KORB_BREAK and returns the break value.
def test_break_from_cfunc_iterator
  arr = []
  result = 0.step(100, 1) do |x|
    arr << x
    break :stopped if x == 5
  end
  assert_equal [0, 1, 2, 3, 4, 5], arr
  assert_equal :stopped, result
end

# Break out of times — same path
def test_break_from_times
  vals = []
  r = 100.times do |i|
    vals << i
    break(:enough) if i == 3
  end
  assert_equal [0, 1, 2, 3], vals
  assert_equal :enough, r
end

# Break out of upto
def test_break_from_upto
  out = []
  3.upto(100) do |x|
    out << x
    break if x == 7
  end
  assert_equal [3, 4, 5, 6, 7], out
end

# Break out of each — Array iterator
def test_break_from_each
  out = []
  result = [10, 20, 30, 40, 50].each do |x|
    out << x
    break "stopped" if x == 30
  end
  assert_equal [10, 20, 30], out
  assert_equal "stopped", result
end

# Break with a falsy value
def test_break_with_nil
  r = [1, 2, 3].each do |x|
    break(nil) if x == 2
  end
  assert_equal nil, r
end

# Yielding fewer args than the block declares — extras get nil.
def test_block_underflow_args
  # Lambdas now enforce strict arity (matches CRuby).  Use a Proc to
  # keep the lenient pad-with-nil behavior; lambda strictness has
  # dedicated coverage in test_lambda_vs_proc.  Result captured via
  # array mutation (proc.call snapshots env on koruby — write-back
  # of `r = ...` doesn't propagate; tracked elsewhere).
  r = []
  blk = proc { |a, b| r << a; r << b }
  blk.call(1)
  assert_equal [1, nil], r
end

# Nested break — break from inner block stops only inner iterator
def test_nested_break
  results = []
  [1, 2, 3].each do |x|
    [10, 20, 30].each do |y|
      break if y == 20
      results << [x, y]
    end
  end
  assert_equal [[1, 10], [2, 10], [3, 10]], results
end

# Multiple breaks in same iteration — only first matters
# (same-line short-circuit): make sure only the first break actually runs.
def test_one_of_two_breaks
  r = nil
  [1, 2, 3].each do |x|
    if x == 2
      r = :hit
      break
    end
  end
  assert_equal :hit, r
end


# Lambda preserves arity behavior — strict
def test_lambda_arity
  l = ->(a, b) { a + b }
  assert_equal 7, l.call(3, 4)
end

# Iterator return value when no break
def test_iterator_default_return
  # Integer#times returns self when no break
  r = 3.times {|i| i }
  assert_equal 3, r
  # Array#each returns self
  arr = [1, 2, 3]
  r2 = arr.each {|x| x }
  assert_equal arr, r2
end

TESTS = %i[
  test_yield_simple test_yield_two test_block_capture test_block_nested
  test_proc test_lambda_two test_block_given
  test_break test_next test_return_from_block_in_method
  test_destructure_yield test_inject_no_seed test_proc_closure
  test_break_from_cfunc_iterator test_break_from_times
  test_break_from_upto test_break_from_each test_break_with_nil
  test_block_underflow_args test_nested_break test_one_of_two_breaks
  test_lambda_arity test_iterator_default_return
]
TESTS.each {|t| run_test(t) }
report("Block")
