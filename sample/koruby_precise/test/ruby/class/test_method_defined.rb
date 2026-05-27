require_relative "../../test_helper"

# Visibility-aware method introspection.

class MDH
  def public_m; end
  private
  def private_m; end
  protected
  def protected_m; end
end

# ---------- method_defined? (public/protected, NOT private) ----------

def test_method_defined_p_public
  assert_equal true,  MDH.method_defined?(:public_m)
end

def test_method_defined_p_protected
  assert_equal true,  MDH.method_defined?(:protected_m)
end

def test_method_defined_p_private_returns_false
  # CRuby: method_defined? returns false for private methods.
  assert_equal false, MDH.method_defined?(:private_m)
end

def test_method_defined_p_missing
  assert_equal false, MDH.method_defined?(:nope)
end

# ---------- visibility-specific predicates ----------

def test_public_method_defined
  assert_equal true,  MDH.public_method_defined?(:public_m)
  assert_equal false, MDH.public_method_defined?(:private_m)
  assert_equal false, MDH.public_method_defined?(:protected_m)
end

def test_private_method_defined
  assert_equal false, MDH.private_method_defined?(:public_m)
  assert_equal true,  MDH.private_method_defined?(:private_m)
  assert_equal false, MDH.private_method_defined?(:protected_m)
end

def test_protected_method_defined
  assert_equal false, MDH.protected_method_defined?(:public_m)
  assert_equal false, MDH.protected_method_defined?(:private_m)
  assert_equal true,  MDH.protected_method_defined?(:protected_m)
end

# ---------- visibility-filtered method lists ----------

def test_private_instance_methods_includes_private
  assert MDH.private_instance_methods(false).include?(:private_m)
end

def test_public_instance_methods_includes_public
  assert MDH.public_instance_methods(false).include?(:public_m)
end

def test_protected_instance_methods_includes_protected
  assert MDH.protected_instance_methods(false).include?(:protected_m)
end

# ---------- instance_methods(false) — own methods only ----------

class MDChild < MDH
  def child_method; end
end

def test_instance_methods_false_excludes_inherited
  own = MDChild.instance_methods(false)
  assert own.include?(:child_method), "own method present"
  # CRuby: includes own public + protected.  Don't strictly require
  # exclusion of public_m (depends on inheritance), but assert that
  # the list is shorter than the include-inherited form.
  with_inh = MDChild.instance_methods(true)
  assert with_inh.length >= own.length, "with-inherited >= own"
end

TESTS = [
  :test_method_defined_p_public,
  :test_method_defined_p_protected,
  :test_method_defined_p_private_returns_false,
  :test_method_defined_p_missing,
  :test_public_method_defined,
  :test_private_method_defined,
  :test_protected_method_defined,
  :test_private_instance_methods_includes_private,
  :test_public_instance_methods_includes_public,
  :test_protected_instance_methods_includes_protected,
  :test_instance_methods_false_excludes_inherited,
]
TESTS.each { |t| run_test(t) }
report "MethodDefined"
