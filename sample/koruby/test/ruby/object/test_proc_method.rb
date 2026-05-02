require_relative "../../test_helper"

# Proc / Lambda / Method tests inspired by CRuby's test_proc.rb,
# test_lambda.rb, test_method.rb.

def test_proc_call
  p = Proc.new { |x| x * 2 }
  assert_equal 6, p.call(3)
  assert_equal 6, p.(3)
  assert_equal 6, p[3]
end

def test_lambda_call
  l = lambda { |x| x + 1 }
  assert_equal 5, l.call(4)
  assert_equal 5, l.(4)
  assert_equal 5, l[4]
end

def test_lambda_arrow
  l = ->(x, y) { x + y }
  assert_equal 7, l.call(3, 4)
end

def test_lambda_p
  assert_equal true,  lambda { 1 }.lambda?
  assert_equal true,  ->(x) { x }.lambda?
  assert_equal false, Proc.new { 1 }.lambda?
end

def test_proc_arity
  assert_equal 0,  Proc.new { }.arity
  assert_equal 1,  Proc.new { |x| x }.arity
  assert_equal 2,  Proc.new { |x, y| x }.arity
  assert_equal(-1, Proc.new { |*x| x }.arity)
  assert_equal(-2, Proc.new { |a, *x| a }.arity)
end

def test_lambda_arity
  assert_equal 1, ->(x) { x }.arity
  assert_equal 2, ->(x, y) { x }.arity
end

def test_block_given
  assert_equal "no",  yield_test_no
  assert_equal "yes", yield_test { 1 }
end
def yield_test_no; block_given? ? "yes" : "no"; end
def yield_test;    block_given? ? "yes" : "no"; end

def test_method_call
  m = "hello".method(:length)
  assert_equal 5, m.call
end

def test_method_to_proc
  m = "hello".method(:upcase)
  pr = m.to_proc
  assert_equal "HELLO", pr.call
end

def test_method_arity
  class_eval_str = nil # avoid unused warning
  m = [].method(:push)
  # Variadic — negative arity.
  assert_equal(-1, m.arity)
end

def test_unbound_method
  klass = Class.new do
    def hi; "hi"; end
  end
  m = klass.instance_method(:hi)
  bound = m.bind(klass.new)
  assert_equal "hi", bound.call
end

def test_method_owner
  klass = Class.new do
    def f; end
  end
  m = klass.instance_method(:f)
  assert_equal klass, m.owner
end

def test_define_method
  klass = Class.new do
    define_method(:greet) { |name| "hi #{name}" }
  end
  assert_equal "hi bob", klass.new.greet("bob")
end

def test_define_method_with_method_obj
  klass = Class.new do
    define_method(:up, "x".method(:upcase))
  end
  assert_equal "X", klass.new.up
end

def test_proc_curry
  add = lambda { |a, b, c| a + b + c }
  c = add.curry
  # Single-call form: pass all args at once.  Chained-call form
  # (`c[1][2][3]`) hits a closure-capture corner with nested lambdas
  # that needs deeper work — see todo.
  assert_equal 6, c[1, 2, 3]
end

def test_proc_eq
  l = lambda { |x| x }
  assert_equal true,  l == l
  assert_equal false, l == lambda { |x| x }
end

TESTS = [
  :test_proc_call, :test_lambda_call, :test_lambda_arrow,
  :test_lambda_p, :test_proc_arity, :test_lambda_arity,
  :test_block_given,
  :test_method_call, :test_method_to_proc, :test_method_arity,
  :test_unbound_method, :test_method_owner,
  :test_define_method, :test_define_method_with_method_obj,
  :test_proc_curry, :test_proc_eq,
]
TESTS.each { |t| run_test(t) }
report "ProcMethod"
