require_relative "../../test_helper"

# Constant lookup, scoping, redefinition.

ALPHA = 1

def test_top_level_const
  assert_equal 1, ALPHA
end

class CLHost
  BETA = 2
  def beta; BETA; end
  def alpha; ALPHA; end          # outer constant visible
end

def test_class_const_lookup
  assert_equal 2, CLHost::BETA
  assert_equal 2, CLHost.new.beta
end

def test_outer_const_visible_from_method
  assert_equal 1, CLHost.new.alpha
end

# ---------- nested ----------

class CLOuter
  CONST = 10
  class Inner
    INNER_CONST = 20
    def both; [CONST, INNER_CONST]; end
  end
end

def test_nested_const_lookup
  assert_equal 20, CLOuter::Inner::INNER_CONST
  assert_equal [10, 20], CLOuter::Inner.new.both
end

# ---------- inheritance ----------

class CLBase
  PARENT_CONST = 100
end

class CLDerived < CLBase
  def lookup; PARENT_CONST; end
end

def test_inherited_const_lookup
  assert_equal 100, CLDerived::PARENT_CONST
  assert_equal 100, CLDerived.new.lookup
end

# ---------- Object#const_get / const_set ----------

class CGHost
  X = 42
end

def test_const_get
  assert_equal 42, CGHost.const_get(:X)
end

def test_const_set
  CGHost.const_set(:Y, 99)
  assert_equal 99, CGHost::Y
end

def test_const_defined
  assert_equal true,  CGHost.const_defined?(:X)
  assert_equal false, CGHost.const_defined?(:Z)
end

TESTS = [
  :test_top_level_const,
  :test_class_const_lookup,
  :test_outer_const_visible_from_method,
  :test_nested_const_lookup,
  :test_inherited_const_lookup,
  :test_const_get,
  :test_const_set,
  :test_const_defined,
]
TESTS.each { |t| run_test(t) }
report "ConstLookup"
