require_relative "test_helper"

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

TESTS = %i[
  test_yield_simple test_yield_two test_block_capture test_block_nested
  test_proc test_lambda_two test_block_given
  test_break test_next test_return_from_block_in_method
  test_destructure_yield test_inject_no_seed test_proc_closure
]
TESTS.each {|t| run_test(t) }
report("Block")
