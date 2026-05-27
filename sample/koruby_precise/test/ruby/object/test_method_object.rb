require_relative "../../test_helper"

# Method objects (Object#method, UnboundMethod proxy, &-prefix to_proc).

# ---------- Object#method ----------

def test_method_call_on_basic_op
  m = 5.method(:+)
  assert_equal 8, m.call(3)
end

def test_method_passes_through_args
  s = "hello"
  m = s.method(:start_with?)
  assert_equal true,  m.call("he")
  assert_equal false, m.call("xx")
end

# ---------- &method ----------

def test_method_as_block_via_amp
  result = [1, 2, 3].map(&:succ)
  assert_equal [2, 3, 4], result
end

def test_method_object_as_block
  doubler = ->(x) { x * 2 }
  assert_equal [2, 4, 6], [1, 2, 3].map(&doubler)
end

# ---------- Method#arity ----------

class MethodArityHost
  def zero_arg; end
  def one_arg(a); a; end
  def two_args(a, b); [a, b]; end
end

def test_method_arity
  o = MethodArityHost.new
  assert_equal 0, o.method(:zero_arg).arity
  assert_equal 1, o.method(:one_arg).arity
  assert_equal 2, o.method(:two_args).arity
end

# ---------- Method object survives assignment ----------

def test_method_object_persists
  m = 10.method(:+)
  v = m.call(5)
  assert_equal 15, v
  # call again — same result (method object is reusable)
  assert_equal 15, m.call(5)
end

TESTS = [
  :test_method_call_on_basic_op,
  :test_method_passes_through_args,
  :test_method_as_block_via_amp,
  :test_method_object_as_block,
  :test_method_arity,
  :test_method_object_persists,
]
TESTS.each { |t| run_test(t) }
report "MethodObject"
