require_relative "../../test_helper"

# Module / Class tests inspired by CRuby's test_module.rb / test_class.rb.

module ME1
  CONST = 42
  def self.shout(s); s.upcase; end
end

def test_module_function
  assert_equal "HI", ME1.shout("hi")
  assert_equal 42,   ME1::CONST
end

class C1
  CC = "value"
  def hello; "hi"; end
end

def test_class_constant
  assert_equal "value", C1::CC
  assert_equal "hi",    C1.new.hello
end

def test_class_name
  assert_equal "C1", C1.name
end

def test_class_superclass
  assert_equal Object, C1.superclass
end

def test_instance_methods
  ms = C1.instance_methods(false)
  assert_equal true, ms.include?(:hello)
end

class C2
end

def test_class_lt
  assert_equal true,  String < Object
  assert_equal false, Object < String
  assert_equal true,  String <= String
end

class C3
  attr_accessor :x
end

def test_attr_accessor
  o = C3.new
  o.x = 5
  assert_equal 5, o.x
end

class C4
  attr_reader :y
  def initialize; @y = 10; end
end

def test_attr_reader
  assert_equal 10, C4.new.y
end

def test_module_module
  assert_equal Module, Module
  assert_equal true,   ME1.is_a?(Module)
end

def test_class_class
  assert_equal Class,  C1.class
end

def test_const_get_set
  C1.const_set(:NEW_C, 99)
  assert_equal 99, C1.const_get(:NEW_C)
end

def test_constants_listing
  cs = C1.constants
  assert_equal true, cs.include?(:CC)
end

def test_class_eval
  C1.class_eval do
    def evaled; "evaled"; end
  end
  assert_equal "evaled", C1.new.evaled
end

def test_method_defined?
  assert_equal true,  C1.method_defined?(:hello)
  assert_equal false, C1.method_defined?(:nope)
end

def test_to_s_class
  assert_equal "Integer", Integer.to_s
end

def test_kind_of
  assert_equal true, "x".kind_of?(String)
  assert_equal true, 3.kind_of?(Integer)
end

TESTS = [
  :test_module_function, :test_class_constant, :test_class_name,
  :test_class_superclass, :test_instance_methods,
  :test_class_lt, :test_attr_accessor, :test_attr_reader,
  :test_module_module, :test_class_class,
  :test_const_get_set, :test_constants_listing,
  :test_class_eval, :test_method_defined?, :test_to_s_class,
  :test_kind_of,
]
TESTS.each { |t| run_test(t) }
report "ModuleMore"
