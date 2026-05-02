require_relative "../../test_helper"

# eval / const tests inspired by CRuby's test_eval.rb / test_const.rb.

def test_eval_basic
  assert_equal 3, eval("1 + 2")
  assert_equal "hi", eval("'hi'")
end

def test_eval_method
  eval("def __evaled_m; :evaled; end")
  assert_equal :evaled, __evaled_m
end

CONST_TOP = 100
def test_top_const
  assert_equal 100, CONST_TOP
end

class CnsA
  CN1 = "a-cn1"
  class B
    CN2 = "b-cn2"
    def lookup; CN1; end          # should find outer scope
    def lookup_b; CN2; end
  end
end

def test_nested_const
  assert_equal "a-cn1", CnsA::B.new.lookup
  assert_equal "b-cn2", CnsA::B.new.lookup_b
end

def test_const_path_get
  assert_equal "a-cn1", CnsA::CN1
  assert_equal "b-cn2", CnsA::B::CN2
end

# defined?
def test_defined_basic
  x = 1
  assert_equal "local-variable", defined?(x)
  assert_equal "expression",     defined?(1 + 2)
  assert_equal nil,              defined?(undefined_xyz)
end

def test_defined_method
  assert_equal "method", defined?(test_eval_basic)
end

def test_defined_const
  assert_equal "constant", defined?(CONST_TOP)
end

def test_class_methods_via_self
  klass = Class.new do
    def self.greet; "hi"; end
  end
  assert_equal "hi", klass.greet
end

# instance_eval and instance_exec
def test_instance_eval
  obj = Object.new
  r = obj.instance_eval { 42 }
  assert_equal 42, r
end

def test_instance_exec_args
  r = "x".instance_exec(3) { |n| length * n }
  assert_equal 3, r
end

TESTS = [
  :test_eval_basic, :test_eval_method,
  :test_top_const, :test_nested_const, :test_const_path_get,
  :test_defined_basic, :test_defined_method, :test_defined_const,
  :test_class_methods_via_self,
  :test_instance_eval, :test_instance_exec_args,
]
TESTS.each { |t| run_test(t) }
report "EvalConst"
