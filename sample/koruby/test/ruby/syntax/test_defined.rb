require_relative "../../test_helper"

def test_defined_lvar
  x = 1
  assert_equal "local-variable", defined?(x)
end

def test_defined_ivar
  assert_equal nil, defined?(@nope)
  @yes = 1
  assert_equal "instance-variable", defined?(@yes)
end

def test_defined_constant
  assert_equal "constant", defined?(Object)
  assert_equal nil, defined?(NoSuchConstantPlease)
end

def test_defined_gvar
  $unset_nothing
  assert_equal nil, defined?($unset_nothing)
  $set_yes = 1
  assert_equal "global-variable", defined?($set_yes)
end

def test_defined_self
  assert_equal "self", defined?(self)
end

def test_defined_literal
  assert_equal "expression", defined?(42)
end

TESTS = [
  :test_defined_lvar, :test_defined_ivar,
  :test_defined_constant, :test_defined_gvar,
  :test_defined_self, :test_defined_literal,
]
TESTS.each { |t| run_test(t) }
report "Defined"
