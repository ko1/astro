require_relative "../../test_helper"

class K
  def double(x); x * 2; end
  def add(a, b); a + b; end
end

def test_method_to_proc_via_amp
  m = K.new.method(:double)
  result = [1, 2, 3].map(&m)
  assert_equal [2, 4, 6], result
end

def test_method_to_proc_call
  m = K.new.method(:add)
  pr = m.to_proc
  assert_equal 7, pr.call(3, 4)
end

def test_method_call_returns_value
  m = K.new.method(:double)
  assert_equal 10, m.call(5)
end

def test_method_arity
  m = K.new.method(:double)
  assert_equal 1, m.arity
  m2 = K.new.method(:add)
  assert_equal 2, m2.arity
end

def test_method_owner
  m = K.new.method(:double)
  assert_equal K, m.owner
end

TESTS = [:test_method_to_proc_via_amp, :test_method_to_proc_call, :test_method_call_returns_value, :test_method_arity, :test_method_owner]
TESTS.each { |t| run_test(t) }
report "MethodToProc"
