require_relative "../../test_helper"

# BasicObject + Kernel inclusion in the ancestor chain.

def test_basic_object_in_ancestors
  assert Object.ancestors.include?(BasicObject), "BasicObject in Object.ancestors"
end

def test_kernel_in_ancestors
  assert Object.ancestors.include?(Kernel), "Kernel in Object.ancestors"
end

def test_ancestors_order
  ancs = Object.ancestors
  o_idx = ancs.index(Object)
  k_idx = ancs.index(Kernel)
  b_idx = ancs.index(BasicObject)
  assert(o_idx < k_idx, "Object before Kernel")
  assert(k_idx < b_idx, "Kernel before BasicObject")
end

def test_integer_ancestors
  ancs = Integer.ancestors
  assert ancs.include?(Integer)
  assert ancs.include?(Numeric)
  assert ancs.include?(Comparable)
  assert ancs.include?(Object)
  assert ancs.include?(BasicObject)
end

def test_basic_object_is_class
  assert BasicObject.is_a?(Class), "BasicObject should be a Class"
end

def test_basic_object_superclass
  # BasicObject has no super class (it's the root)
  assert_equal nil, BasicObject.superclass
end

def test_object_superclass_is_basic_object
  assert_equal BasicObject, Object.superclass
end

TESTS = [
  :test_basic_object_in_ancestors,
  :test_kernel_in_ancestors,
  :test_ancestors_order,
  :test_integer_ancestors,
  :test_basic_object_is_class,
  :test_basic_object_superclass,
  :test_object_superclass_is_basic_object,
]
TESTS.each { |t| run_test(t) }
report "BasicObject"
