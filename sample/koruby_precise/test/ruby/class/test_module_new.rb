require_relative "../../test_helper"

def test_module_new_with_block
  m = Module.new do
    def hi; "hello"; end
  end
  k = Class.new
  k.include(m)
  assert_equal "hello", k.new.hi
end

def test_class_new_with_block_def
  k = Class.new do
    def double(x); x * 2; end
  end
  assert_equal 6, k.new.double(3)
end

def test_class_new_with_super
  parent = Class.new
  parent.define_method(:hello) { "hi" }
  child = Class.new(parent)
  assert_equal "hi", child.new.hello
end

def test_module_new_no_block
  m = Module.new
  assert_equal Module, m.class
end

TESTS = [:test_module_new_with_block, :test_class_new_with_block_def,
          :test_class_new_with_super, :test_module_new_no_block]
TESTS.each { |t| run_test(t) }
report "ModuleNew"
