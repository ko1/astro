require_relative "../../test_helper"

def test_loop_with_break
  count = 0
  loop do
    count += 1
    break if count >= 3
  end
  assert_equal 3, count
end

def test_proc
  pr = proc { |x| x * 2 }
  assert_equal 6, pr.call(3)
end

def test_lambda
  pr = lambda { |x| x + 1 }
  assert_equal 5, pr.call(4)
end

def test_lambda_arrow
  pr = ->(x) { x * 10 }
  assert_equal 50, pr.call(5)
end

def test_kernel_array
  assert_equal [1, 2, 3], Array([1, 2, 3])
  assert_equal [], Array(nil)
  # Array(scalar) wraps in array
  assert_equal [42], Array(42)
end

def test_kernel_integer
  assert_equal 42, Integer("42")
  assert_equal 42, Integer(42)
end

def test_kernel_string
  assert_equal "42", String(42)
  assert_equal "abc", String("abc")
end

TESTS = [
  :test_loop_with_break,
  :test_proc,
  :test_lambda,
  :test_lambda_arrow,
  :test_kernel_array,
  :test_kernel_integer,
  :test_kernel_string,
]

TESTS.each { |t| run_test(t) }
report "KernelExtra"
