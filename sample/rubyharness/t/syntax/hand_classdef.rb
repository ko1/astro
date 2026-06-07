# syntax (hand): class / module declaration forms
class Foo
  def bar; 1; end
end
p Foo.new.bar

class Foo            # reopen
  def baz; 2; end
end
p Foo.new.baz
p Foo.new.bar

class Sub < Foo; end
p Sub.new.bar

module M
  def hello; "hi"; end
end
class Inc
  include M
end
p Inc.new.hello
p Inc.ancestors.include?(M)

obj = Object.new
class << obj
  def special; 42; end
end
p obj.special

Klass = Class.new do
  def x; 7; end
end
p Klass.new.x

Mod = Module.new do
  def y; 8; end
end
class UsesMod
  include Mod
end
p UsesMod.new.y

class Outer
  class Inner
    VAL = 99
  end
end
p Outer::Inner::VAL

class WithClassMethods
  C = 10
  def self.cls_method; "from class"; end
  class << self
    def another; "singleton block"; end
  end
end
p WithClassMethods::C
p WithClassMethods.cls_method
p WithClassMethods.another

class Empty; end
p Empty.new.is_a?(Empty)

class Base
  def greet; "base"; end
end
class Derived < Base
  def greet; super + "+derived"; end
end
p Derived.new.greet
