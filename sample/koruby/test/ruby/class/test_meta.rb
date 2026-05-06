require_relative "../../test_helper"

# Metaprogramming smoke tests — class variables, exec families, hooks,
# Module.nesting, singleton_class, allocate, define_method-with-Method,
# Comparable mixin.

# ---------- @@class variables ----------

class Counter
  @@count = 0
  def self.bump; @@count += 1; end
  def self.count; @@count; end
end

def test_class_variable_basic
  Counter.class_variable_set(:@@count, 0)
  Counter.bump
  Counter.bump
  Counter.bump
  assert_equal 3, Counter.count
  assert_equal 3, Counter.class_variable_get(:@@count)
  assert Counter.class_variable_defined?(:@@count)
  assert !Counter.class_variable_defined?(:@@nope)
  assert_equal [:@@count], Counter.class_variables
end

# ---------- class_exec / module_exec / instance_exec ----------

def test_class_exec_with_args
  klass = Class.new
  klass.class_exec(7) do |x|
    define_method(:n) { x }
  end
  assert_equal 7, klass.new.n
end

def test_instance_exec_with_args
  o = Object.new
  o.instance_variable_set(:@n, 100)
  r = o.instance_exec(7) { |x| @n + x }
  assert_equal 107, r
end

def test_module_exec_alias
  mod = Module.new
  mod.module_exec do
    def hi; :hi; end
  end
  k = Class.new { include mod }
  assert_equal :hi, k.new.hi
end

# ---------- Hooks ----------

def test_inherited_hook
  parent = Class.new do
    @@kids = []
    def self.inherited(child); @@kids << child; end
    def self.kids; @@kids; end
  end
  Class.new(parent)
  Class.new(parent)
  assert_equal 2, parent.kids.size
end

def test_included_hook
  m = Module.new do
    @@inc = []
    def self.included(klass); @@inc << klass; end
    def self.inc; @@inc; end
  end
  Class.new { include m }
  assert_equal 1, m.inc.size
end

def test_extended_hook
  m = Module.new do
    @@ext = []
    def self.extended(obj); @@ext << obj; end
    def self.ext; @@ext; end
  end
  Object.new.extend(m)
  assert_equal 1, m.ext.size
end

def test_method_added_hook
  klass = Class.new do
    @@added = []
    def self.method_added(name); @@added << name; end
    def self.added; @@added; end
    def foo; end
    def bar; end
  end
  assert klass.added.include?(:foo)
  assert klass.added.include?(:bar)
end

# ---------- Module.nesting ----------

module N1
  module N2
    NESTING = Module.nesting
  end
end

def test_module_nesting
  assert_equal 2, N1::N2::NESTING.size
  # Order: innermost first.
  assert_equal N1::N2, N1::N2::NESTING[0]
  assert_equal N1,     N1::N2::NESTING[1]
end

# ---------- singleton_class ----------

def test_singleton_class_define_method
  o = Object.new
  o.singleton_class.define_method(:secret) { :only_me }
  assert_equal :only_me, o.secret
  o2 = Object.new
  assert !o2.respond_to?(:secret)
end

# ---------- Class#allocate ----------

class AllocBox
  def initialize; @x = 1; end
end

def test_class_allocate_skips_initialize
  o = AllocBox.allocate
  # initialize ran for normal `.new` instances, but not for allocate.
  assert !o.instance_variable_defined?(:@x)
end

# ---------- define_method with Method/UnboundMethod ----------

class SrcK
  def g; :src_g; end
end

def test_define_method_with_unbound_method
  k = Class.new
  um = SrcK.instance_method(:g)
  k.define_method(:g, um)
  assert_equal :src_g, k.new.g
end

def test_unbound_method_bind_call
  um = SrcK.instance_method(:g)
  bound = um.bind(SrcK.new)
  assert_equal :src_g, bound.call
end

# ---------- const_missing ----------

class CMHost
  def self.const_missing(name); "CM:#{name}"; end
end

def test_const_missing
  assert_equal "CM:NOPE", CMHost::NOPE
end

# ---------- Comparable mixin via include ----------

class CmpC
  include Comparable
  attr_reader :v
  def initialize(v); @v = v; end
  def <=>(o); @v <=> o.v; end
end

def test_comparable_mixin
  a, b, c = CmpC.new(1), CmpC.new(2), CmpC.new(3)
  assert a < b
  assert b < c
  assert_equal a, [c, a, b].min
  assert_equal c, [c, a, b].max
end

TESTS = %i[
  test_class_variable_basic
  test_class_exec_with_args test_instance_exec_with_args test_module_exec_alias
  test_inherited_hook test_included_hook test_extended_hook test_method_added_hook
  test_module_nesting test_singleton_class_define_method
  test_class_allocate_skips_initialize
  test_define_method_with_unbound_method test_unbound_method_bind_call
  test_const_missing test_comparable_mixin
]
TESTS.each {|t| run_test(t) }
report "Meta"
