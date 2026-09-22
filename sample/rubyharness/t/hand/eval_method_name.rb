# `__method__` inside eval(str, binding) names the method the BINDING was taken
# in — the explicit-receiver form too (the bare one is baked at parse time).
class Foo
  def bar = binding
  def with_arg(x) = binding
  def baz = [__method__, ::Kernel.__method__, Kernel.__method__]
end
b = Foo.new.bar
p [b.eval('__method__'), b.eval('::Kernel.__method__'), b.eval('Kernel.__method__')]
p b.eval('self.__method__') rescue p [$!.class, $!.message[0, 30]]
p Foo.new.with_arg(1).eval('::Kernel.__method__')
p Foo.new.baz
def top = [__method__, ::Kernel.__method__]
p top
p [__method__, ::Kernel.__method__]
p TOPLEVEL_BINDING.eval('::Kernel.__method__')
p binding.eval('::Kernel.__method__')
# a nested eval restores the outer answer
class Deep
  def m
    b2 = binding
    [b2.eval('::Kernel.__method__'), b2.eval('binding.eval("::Kernel.__method__")')]
  end
end
p Deep.new.m
p (Kernel.instance_method(:method).bind(b.eval('self')).call(b.eval('::Kernel.__method__')).name)

# a binding CAPTURED inside an eval'd string keeps the method name it was taken
# in (pry's Object#__binding__ is `class_eval("binding")` inside a method)
class Obj2
  def grab
    (class << self; self; end).class_eval("def __pb__; binding; end", __FILE__, __LINE__)
    __pb__
  end
  def grab2 = self.class.class_eval("binding", __FILE__, __LINE__)
end
p Obj2.new.grab.eval("::Kernel.__method__")
p Obj2.new.grab2.eval("::Kernel.__method__")
module Mod2
  def self.grab = class_eval("binding", __FILE__, __LINE__)
end
p Mod2.grab.eval("::Kernel.__method__")
