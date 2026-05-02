require_relative "../../test_helper"

class P
  def initialize(x, y)
    @x = x
    @y = y
  end
end

def test_instance_variable_get
  p = P.new(1, 2)
  assert_equal 1, p.instance_variable_get(:@x)
  assert_equal 2, p.instance_variable_get(:@y)
end

def test_instance_variable_get_string
  p = P.new(3, 4)
  assert_equal 3, p.instance_variable_get("@x")
end

def test_instance_variable_set
  p = P.new(1, 2)
  p.instance_variable_set(:@x, 99)
  assert_equal 99, p.instance_variable_get(:@x)
end

def test_instance_variables_list
  p = P.new(1, 2)
  ivars = p.instance_variables
  assert_equal true, ivars.include?(:@x)
  assert_equal true, ivars.include?(:@y)
end

def test_instance_variable_defined
  p = P.new(1, 2)
  assert_equal true, p.instance_variable_defined?(:@x)
  assert_equal false, p.instance_variable_defined?(:@nope)
end

TESTS = [
  :test_instance_variable_get,
  :test_instance_variable_get_string,
  :test_instance_variable_set,
  :test_instance_variables_list,
  :test_instance_variable_defined,
]

TESTS.each { |t| run_test(t) }
report "IvarIntrospect"
