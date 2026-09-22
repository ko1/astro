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
