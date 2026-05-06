require_relative "../../test_helper"

# Proc#curry — partial-application chain.

def test_lambda_curry_chain
  add = ->(a, b, c) { a + b + c }
  c0 = add.curry
  c1 = c0[1]
  c2 = c1[2]
  c3 = c2[3]
  assert_equal 6, c3
end

def test_lambda_curry_inline
  add = ->(a, b, c) { a + b + c }
  assert_equal 6, add.curry[1][2][3]
end

def test_lambda_curry_multi_args
  add5 = ->(a, b, c, d, e) { a + b + c + d + e }
  assert_equal 15, add5.curry[1, 2][3, 4][5]
  assert_equal 15, add5.curry[1, 2, 3][4, 5]
  assert_equal 15, add5.curry[1, 2, 3, 4, 5]
end

def test_proc_curry_with_arity_arg
  f = ->(*xs) { xs.sum }
  c = f.curry(3)
  assert_equal 6, c[1][2][3]
end

def addm(a, b); a + b; end
def test_method_curry
  m = method(:addm)
  assert_equal 2, m.arity
  c = m.curry
  assert_equal 30, c[10][20]
end

def test_proc_curry_returns_proc_until_full
  add = ->(a, b, c) { a + b + c }
  c0 = add.curry
  assert(c0.is_a?(Proc), "curry returns Proc")
  c1 = c0[1]
  assert(c1.is_a?(Proc), "[1] still Proc")
  c2 = c1[2]
  assert(c2.is_a?(Proc), "[1][2] still Proc")
  c3 = c2[3]
  assert_equal 6, c3
end

# Plain (non-lambda) Proc.curry
def test_proc_new_curry
  pr = Proc.new { |a, b, c| a + b + c }
  assert_equal 6, pr.curry[1][2][3]
end

TESTS = %i[
  test_lambda_curry_chain test_lambda_curry_inline
  test_lambda_curry_multi_args test_proc_curry_with_arity_arg
  test_method_curry test_proc_curry_returns_proc_until_full
  test_proc_new_curry
]
TESTS.each {|t| run_test(t) }
report "ProcCurry"
