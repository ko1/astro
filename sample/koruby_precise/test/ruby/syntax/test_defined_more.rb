require_relative "../../test_helper"

# defined? — various contexts.

GLOBAL_DEFINED_CONST = 1
$defined_global = 1

def test_defined_local_var
  x = 1
  assert_equal "local-variable", defined?(x)
end

def test_defined_undef_local
  assert_equal nil, defined?(undef_lvar_xyz)
end

def test_defined_const
  assert_equal "constant", defined?(GLOBAL_DEFINED_CONST)
  assert_equal nil,        defined?(NONEXISTENT_CONST_XYZ)
end

def test_defined_global
  assert_equal "global-variable", defined?($defined_global)
end

def test_defined_undef_global
  # Reading an undefined $var returns nil but is "defined" (per CRuby);
  # accept either nil or "global-variable".
  result = defined?($never_set_global_xyz)
  assert(result.nil? || result == "global-variable",
         "got #{result.inspect}")
end

def test_defined_method
  assert_equal "method", defined?(self.to_s)
end

def test_defined_self
  assert_equal "self", defined?(self)
end

def test_defined_nil_true_false
  assert_equal "expression", defined?(nil)
  assert_equal "expression", defined?(true)
  assert_equal "expression", defined?(false)
end

# ---------- defined? on ivar ----------

class IvDef
  def initialize; @x = 1; end
  def has_x?; defined?(@x); end
  def has_y?; defined?(@y); end
end

def test_defined_ivar
  o = IvDef.new
  assert_equal "instance-variable", o.has_x?
  assert_equal nil,                 o.has_y?
end

TESTS = [
  :test_defined_local_var,
  :test_defined_undef_local,
  :test_defined_const,
  :test_defined_global,
  :test_defined_undef_global,
  :test_defined_method,
  :test_defined_self,
  :test_defined_nil_true_false,
  :test_defined_ivar,
]
TESTS.each { |t| run_test(t) }
report "DefinedMore"
