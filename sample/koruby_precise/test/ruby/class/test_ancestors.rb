# Tests for Module#ancestors / is_a? — common introspection.

require_relative "../../test_helper"

module M; end
module N; end

class A; end
class B < A
  include M
end
class C < B
  include N
end

def test_class_ancestors_basic
  anc = A.ancestors
  assert_equal true, anc.include?(A)
  assert_equal true, anc.include?(Object)
end

def test_subclass_ancestors_includes_parent
  anc = B.ancestors
  assert_equal true, anc.include?(B)
  assert_equal true, anc.include?(A)
  assert_equal true, anc.include?(M)
  assert_equal true, anc.include?(Object)
end

def test_ancestors_order
  anc = C.ancestors
  # subclass first, then included module, then super, then super's module
  ci = anc.index(C); ni = anc.index(N); bi = anc.index(B); mi = anc.index(M); ai = anc.index(A)
  assert_equal true, ci < ni
  assert_equal true, ni < bi
  assert_equal true, bi < mi
  assert_equal true, mi < ai
end

def test_module_ancestors
  anc = M.ancestors
  assert_equal true, anc.include?(M)
end

def test_is_a_with_module
  c = C.new
  assert_equal true, c.is_a?(C)
  assert_equal true, c.is_a?(B)
  assert_equal true, c.is_a?(A)
  assert_equal true, c.is_a?(M)
  assert_equal true, c.is_a?(N)
  assert_equal true, c.is_a?(Object)
end

def test_kind_of_aliased
  c = C.new
  assert_equal true, c.kind_of?(M)
  assert_equal false, c.kind_of?(String)
end

def test_class_method
  assert_equal C, C.new.class
  assert_equal Array, [].class
end

TESTS = [
  :test_class_ancestors_basic,
  :test_subclass_ancestors_includes_parent,
  :test_ancestors_order,
  :test_module_ancestors,
  :test_is_a_with_module,
  :test_kind_of_aliased,
  :test_class_method,
]

TESTS.each { |t| run_test(t) }
report "Ancestors"
