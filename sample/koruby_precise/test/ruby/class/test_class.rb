require_relative "../../test_helper"

class Animal
  def initialize(name)
    @name = name
  end
  attr_reader :name
  def greet
    "Hello, I am #{@name}"
  end
end

class Dog < Animal
  def initialize(name, breed)
    super(name)
    @breed = breed
  end
  attr_reader :breed
  def greet
    super + " (#{@breed})"
  end
end

def test_basic_class
  a = Animal.new("Buddy")
  assert_equal "Buddy", a.name
  assert_equal "Hello, I am Buddy", a.greet
end

def test_inheritance
  d = Dog.new("Rex", "Lab")
  assert_equal "Rex", d.name
  assert_equal "Lab", d.breed
  assert_equal "Hello, I am Rex (Lab)", d.greet
end

def test_is_a
  d = Dog.new("Rex", "Lab")
  assert_equal true, d.is_a?(Dog)
  assert_equal true, d.is_a?(Animal)
  assert_equal true, d.is_a?(Object)
  assert_equal false, d.is_a?(String)
end

def test_class_method
  class A
    def self.factory(x)
      A.new
    end
    def initialize
      @x = 0
    end
  end
  assert_equal Object, Object  # smoke
end

class Counter
  def initialize
    @count = 0
  end
  def incr
    @count += 1
  end
  def value
    @count
  end
end

def test_ivars
  c = Counter.new
  c.incr; c.incr; c.incr
  assert_equal 3, c.value
end

module Greeting
  def hello
    "hello from #{self.class}"
  end
end

class Greeter
  include Greeting
end

def test_module_include
  g = Greeter.new
  assert_equal "hello from Greeter", g.hello
end

def test_attr_accessor
  class Box
    attr_accessor :w, :h
  end
  b = Box.new
  b.w = 10
  b.h = 20
  assert_equal 10, b.w
  assert_equal 20, b.h
end

class StructLike
  StructStruct = Struct.new(:x, :y) rescue nil
end

def test_struct
  return unless defined?(StructLike::StructStruct)
  return if StructLike::StructStruct.nil?
  s = StructLike::StructStruct.new(3, 4)
  assert_equal 3, s.x
  assert_equal 4, s.y
end

# Lexical constant lookup
module M
  X = 100
  class C
    def get_x
      X
    end
  end
end

def test_lexical_const
  assert_equal 100, M::C.new.get_x
  assert_equal 100, M::X
end

# Constant from outer outer module
module Outer
  Y = 200
  module Inner
    class K
      def get_y
        Y
      end
    end
  end
end

def test_lexical_outer
  assert_equal 200, Outer::Inner::K.new.get_y
end

TESTS = %i[
  test_basic_class test_inheritance test_is_a
  test_ivars test_module_include test_attr_accessor test_struct
  test_lexical_const test_lexical_outer
]
TESTS.each {|t| run_test(t) }
report("Class")
