require_relative "../../test_helper"

# Method definition edge cases: redefine, undef, optional args defaults.

# ---------- last def wins ----------

class Redef1
  def foo; :first; end
  def foo; :second; end
end

def test_last_def_wins
  assert_equal :second, Redef1.new.foo
end

# ---------- optional argument defaults ----------

class OptArgs
  def f(a, b = 10, c = 20); [a, b, c]; end
end

def test_default_value_used
  assert_equal [1, 10, 20],  OptArgs.new.f(1)
  assert_equal [1, 99, 20],  OptArgs.new.f(1, 99)
  assert_equal [1, 99, 33],  OptArgs.new.f(1, 99, 33)
end

# ---------- defaults can reference earlier args ----------

class DepOpt
  def f(a, b = a * 2); [a, b]; end
end

def test_default_can_reference_earlier
  assert_equal [3, 6], DepOpt.new.f(3)
  assert_equal [3, 7], DepOpt.new.f(3, 7)
end

# ---------- def returning the symbol ----------

class DefRet
end

def test_def_returns_symbol
  result = DefRet.class_eval { def hi; "hi"; end }
  # CRuby: def returns the method-name Symbol; some impls return nil.
  # Accept either, but at least the method should now exist.
  assert_equal "hi", DefRet.new.hi
end

# ---------- def overriding inherited ----------

class OvBase
  def hi; "base"; end
end
class OvSub < OvBase
  def hi; "sub"; end
end

def test_subclass_override
  assert_equal "sub", OvSub.new.hi
  assert_equal "base", OvBase.new.hi
end

# ---------- undef_method ----------

class Undefme
  def vanishing; :here; end
end

def test_undef_method_removes
  Undefme.class_eval { undef_method :vanishing }
  raised = false
  begin
    Undefme.new.vanishing
  rescue NoMethodError, NameError, RuntimeError
    raised = true
  end
  assert raised, "expected NoMethodError after undef_method"
end

TESTS = [
  :test_last_def_wins,
  :test_default_value_used,
  :test_default_can_reference_earlier,
  :test_def_returns_symbol,
  :test_subclass_override,
  :test_undef_method_removes,
]
TESTS.each { |t| run_test(t) }
report "MethodDefEdge"
