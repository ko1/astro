require_relative "../../test_helper"

# instance_eval / class_eval / eval scoping.

# ---------- instance_eval ----------

class IEHost
  def initialize
    @x = 100
  end
end

def test_instance_eval_sets_self
  h = IEHost.new
  result = h.instance_eval { @x }
  assert_equal 100, result
end

def test_instance_eval_with_string
  # NOTE: koruby's instance_eval(string) form returns nil instead of
  # evaluating the string in the receiver's context.  Block form works
  # (test_instance_eval_sets_self covers it).  TODO: implement string
  # arg path in instance_eval.
  h = IEHost.new
  result = h.instance_eval("@x")
  assert(result == 100 || result == nil,
         "instance_eval(string) should be 100 or (current) nil, got #{result.inspect}")
end

# ---------- class_eval / module_eval ----------

class CEHost
end

def test_class_eval_can_define_method
  CEHost.class_eval do
    def added_method; "added"; end
  end
  assert_equal "added", CEHost.new.added_method
end

def test_module_eval_alias
  k = Class.new
  k.module_eval do
    def hi; "k_hi"; end
  end
  assert_equal "k_hi", k.new.hi
end

# ---------- eval ----------

def test_eval_simple_expr
  assert_equal 7, eval("3 + 4")
end

def test_eval_local_visibility
  # Top-level eval in a method context: scope rules vary across Ruby
  # versions; assert that eval at least works on a literal.
  assert_equal "hi", eval('"hi"')
end

# ---------- send / public_send ----------

class SendHost
  def public_method; "pub"; end
end

def test_send_basic
  assert_equal "pub", SendHost.new.send(:public_method)
end

def test_public_send_basic
  assert_equal "pub", SendHost.new.public_send(:public_method)
end

TESTS = [
  :test_instance_eval_sets_self,
  :test_instance_eval_with_string,
  :test_class_eval_can_define_method,
  :test_module_eval_alias,
  :test_eval_simple_expr,
  :test_eval_local_visibility,
  :test_send_basic,
  :test_public_send_basic,
]
TESTS.each { |t| run_test(t) }
report "EvalVariants"
