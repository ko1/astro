require_relative "../../test_helper"

# Class-level @ivars, module include order, super through modules.

class CIvHost
  @x = 42
  @y = "hello"

  def self.show_x; @x; end
  def self.show_y; @y; end
  def self.set_x(v); @x = v; end
end

def test_class_ivar_set_in_body
  assert_equal 42, CIvHost.show_x
end

def test_class_ivar_set_via_method
  CIvHost.set_x(99)
  assert_equal 99, CIvHost.show_x
end

def test_instance_variable_get_on_class
  assert_equal "hello", CIvHost.instance_variable_get(:@y)
end

# ---------- Module include order: M2 wins over M1 ----------

module InM1
  def x; "M1"; end
  def y; "M1-y"; end
end

module InM2
  def x; "M2-" + super; end  # overrides x, calls into M1's x
end

class InHost
  include InM1
  include InM2
end

def test_module_include_order
  assert_equal "M2-M1", InHost.new.x
end

def test_module_super_to_earlier_include
  # InM1's y is still reachable since InM2 doesn't override.
  assert_equal "M1-y", InHost.new.y
end

def test_module_ancestors_order
  # CRuby: [InHost, InM2, InM1, Object, Kernel, BasicObject]
  ancestors = InHost.ancestors
  assert_equal InHost, ancestors[0]
  assert_equal InM2, ancestors[1]
  assert_equal InM1, ancestors[2]
end

# ---------- Class-defined methods beat included modules ----------

module BeatM
  def name; "from-module"; end
end

class BeatHost
  def name; "from-class"; end
  include BeatM
end

def test_class_method_beats_module
  # Including a module never overrides a method defined directly on
  # the class, regardless of whether the include came first or last.
  assert_equal "from-class", BeatHost.new.name
end

# ---------- Reverse order: include then class def overrides ----------

module RevM
  def thing; "module"; end
end

class RevHost
  include RevM
  def thing; "class-#{super}"; end  # def after include — class wins, super reaches module
end

def test_class_def_after_include
  assert_equal "class-module", RevHost.new.thing
end

TESTS = [
  :test_class_ivar_set_in_body,
  :test_class_ivar_set_via_method,
  :test_instance_variable_get_on_class,
  :test_module_include_order,
  :test_module_super_to_earlier_include,
  :test_module_ancestors_order,
  :test_class_method_beats_module,
  :test_class_def_after_include,
]
TESTS.each { |t| run_test(t) }
report "ClassIvarsAndIncludes"
