# Tests for Module#define_method — runtime method definition with a block.

require_relative "../../test_helper"

class A
  define_method(:hello) { "hello" }
  define_method(:double) { |x| x * 2 }
  define_method(:add) { |x, y| x + y }
end

def test_define_method_no_args
  assert_equal "hello", A.new.hello
end

def test_define_method_with_arg
  assert_equal 6, A.new.double(3)
end

def test_define_method_two_args
  assert_equal 7, A.new.add(3, 4)
end

# define_method captures the surrounding scope — prove it
def make_class(prefix)
  k = Class.new
  k.define_method(:greet) { |name| "#{prefix}, #{name}" }
  k
end

def test_define_method_captures_scope
  k = make_class("Hi")
  obj = k.new
  assert_equal "Hi, World", obj.greet("World")
end

# define_method returns the symbol of the defined name
def test_define_method_returns_symbol
  k = Class.new
  result = k.define_method(:foo) { 1 }
  assert_equal :foo, result
end

TESTS = [
  :test_define_method_no_args,
  :test_define_method_with_arg,
  :test_define_method_two_args,
  :test_define_method_captures_scope,
  :test_define_method_returns_symbol,
]

TESTS.each { |t| run_test(t) }
report "DefineMethod"
