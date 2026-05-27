require_relative "../../test_helper"

# Kernel#binding — koruby provides a manual-population Binding (no
# auto-capture of caller lvars; values must be set via
# local_variable_set) plus binding.eval(src).

def test_binding_returns_binding
  b = binding
  assert(b.is_a?(Binding))
end

def test_local_variable_set_get
  b = binding
  b.local_variable_set(:x, 42)
  assert_equal 42, b.local_variable_get(:x)
end

def test_local_variables
  b = binding
  b.local_variable_set(:added_a, 1)
  b.local_variable_set(:added_b, 2)
  # binding auto-captures the caller's named lvars too — `b` itself
  # is in the local list.  Just assert our manually-set keys appear.
  assert b.local_variables.include?(:added_a)
  assert b.local_variables.include?(:added_b)
end

def test_binding_auto_captures_lvars
  x = 10
  y = 20
  z = "str"
  b = binding
  assert_equal 10,    b.local_variable_get(:x)
  assert_equal 20,    b.local_variable_get(:y)
  assert_equal "str", b.local_variable_get(:z)
end

def test_binding_eval_sees_auto_captured_lvars
  a = 5
  b_val = 7
  bnd = binding
  assert_equal 35, bnd.eval("a * b_val")
end

def test_local_variable_defined_p
  b = binding
  b.local_variable_set(:x, 1)
  assert b.local_variable_defined?(:x)
  assert !b.local_variable_defined?(:y)
end

def test_eval_simple_arithmetic
  b = binding
  assert_equal 3, b.eval("1 + 2")
end

def test_eval_with_set_variable
  b = binding
  b.local_variable_set(:n, 10)
  assert_equal 100, b.eval("n * n")
end

def test_eval_with_array_value
  b = binding
  b.local_variable_set(:list, [1, 2, 3])
  assert_equal 6, b.eval("list.sum")
end

def test_eval_with_hash_value
  b = binding
  b.local_variable_set(:h, {a: 1, b: 2})
  assert_equal 3, b.eval("h[:a] + h[:b]")
end

def test_eval_undefined_var_raises
  b = binding
  raised = false
  begin
    b.local_variable_get(:nope)
  rescue NameError
    raised = true
  end
  assert raised
end

def test_binding_receiver
  b = binding
  # Receiver should be the caller's `self` (the Main toplevel object
  # for tests, or the test method's self).  Just check it's not nil.
  assert(b.receiver != nil)
end

TESTS = %i[
  test_binding_returns_binding
  test_local_variable_set_get test_local_variables
  test_local_variable_defined_p
  test_eval_simple_arithmetic test_eval_with_set_variable
  test_eval_with_array_value test_eval_with_hash_value
  test_eval_undefined_var_raises test_binding_receiver
  test_binding_auto_captures_lvars
  test_binding_eval_sees_auto_captured_lvars
]
TESTS.each {|t| run_test(t) }
report "Binding"
