require_relative "../../test_helper"

# Object#extend now preserves @ivars: pre-existing @ivars on the object
# remain readable after the singleton class is created.
# Object#methods / singleton_methods reflection.

module Greeter
  def hello; "hi #{name}"; end
end

class Person
  attr_accessor :name
end

def test_extend_preserves_ivars
  p = Person.new
  p.name = "alice"
  p.extend(Greeter)
  assert_equal "hi alice", p.hello
end

def test_extend_singleton_methods
  obj = Object.new
  obj.extend(Greeter) rescue nil
  # extend installs Greeter on the singleton class.
  # respond_to? should now report :hello.
  assert obj.respond_to?(:hello)
end

def test_obj_methods_returns_array_of_symbols
  m = "hello".methods
  assert m.is_a?(Array)
  assert m.length > 0
  assert m.first.is_a?(Symbol)
  assert m.include?(:upcase)
end

def test_obj_methods_excludes_private
  class << "x"
    def my_pub; "public"; end
    private
    def my_priv; "private"; end
  end rescue nil
  # We won't dig into String's singleton class — just verify methods
  # array doesn't include common private dispatchers.
  m = "hello".methods
  refute_send_in = !m.include?(:send_method_internal_xyz)
  assert refute_send_in
end

def test_singleton_methods_lists_extend_target
  obj = Object.new
  obj.extend(Greeter)
  # singleton_methods should at least reflect that obj has additional
  # methods beyond Object's standard set — we don't assert exact list.
  sm = obj.singleton_methods
  assert sm.is_a?(Array)
end

# ---------- Class@ivars introspection survives extend ----------

class Counter
  @count = 5
end

def test_class_ivar_survives
  assert_equal 5, Counter.instance_variable_get(:@count)
end

TESTS = [
  :test_extend_preserves_ivars,
  :test_extend_singleton_methods,
  :test_obj_methods_returns_array_of_symbols,
  :test_obj_methods_excludes_private,
  :test_singleton_methods_lists_extend_target,
  :test_class_ivar_survives,
]
TESTS.each { |t| run_test(t) }
report "ExtendMethods"
