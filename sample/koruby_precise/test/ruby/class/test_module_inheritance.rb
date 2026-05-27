require_relative "../../test_helper"

# Module#include / inheritance / ancestors / method_defined?.

module M1
  def m1_method; "M1"; end
end

module M2
  def m2_method; "M2"; end
end

class A_inh
  include M1
end

class B_inh < A_inh
  include M2
end

def test_include_method_visible
  assert_equal "M1", A_inh.new.m1_method
end

def test_inherits_through_include
  b = B_inh.new
  assert_equal "M1", b.m1_method
  assert_equal "M2", b.m2_method
end

# ---------- ancestors ordering ----------

def test_ancestors_include_modules
  ancs = B_inh.ancestors
  assert ancs.include?(M2),    "M2 should be in ancestors"
  assert ancs.include?(A_inh), "A_inh should be in ancestors"
  assert ancs.include?(M1),    "M1 should be in ancestors via A_inh"
end

def test_ancestors_order_class_before_super
  # B_inh should come before A_inh in its own ancestors.
  ancs = B_inh.ancestors
  bi = ancs.index(B_inh)
  ai = ancs.index(A_inh)
  assert(bi != nil && ai != nil, "expected both classes in ancestors")
  assert(bi < ai, "B_inh should precede A_inh in ancestors")
end

# ---------- multiple includes ----------

class C_multi
  include M1
  include M2
end

def test_multiple_includes_both_visible
  c = C_multi.new
  assert_equal "M1", c.m1_method
  assert_equal "M2", c.m2_method
end

# ---------- method_defined? ----------

def test_method_defined_p
  assert_equal true,  A_inh.method_defined?(:m1_method)
  assert_equal false, A_inh.method_defined?(:nonexistent_method)
  # Inherited methods count too.
  assert_equal true,  B_inh.method_defined?(:m1_method)
end

# ---------- Class.new with body ----------

def test_class_new_with_body
  k = Class.new do
    def hi; "hi"; end
  end
  assert_equal "hi", k.new.hi
end

# ---------- superclass ----------

def test_superclass
  assert_equal A_inh, B_inh.superclass
  assert_equal Object, A_inh.superclass
end

TESTS = [
  :test_include_method_visible,
  :test_inherits_through_include,
  :test_ancestors_include_modules,
  :test_ancestors_order_class_before_super,
  :test_multiple_includes_both_visible,
  :test_method_defined_p,
  :test_class_new_with_body,
  :test_superclass,
]
TESTS.each { |t| run_test(t) }
report "ModuleInheritance"
