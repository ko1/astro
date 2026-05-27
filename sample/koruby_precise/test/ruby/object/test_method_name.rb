require_relative "../../test_helper"

def my_func
  __method__
end

class K
  def hello
    __method__
  end
end

def test_method_name_in_func
  assert_equal :my_func, my_func
end

def test_method_name_in_method
  assert_equal :hello, K.new.hello
end

def callee_demo
  __callee__
end

def test_callee_alias
  assert_equal :callee_demo, callee_demo
end

TESTS = [:test_method_name_in_func, :test_method_name_in_method, :test_callee_alias]
TESTS.each { |t| run_test(t) }
report "MethodName"
