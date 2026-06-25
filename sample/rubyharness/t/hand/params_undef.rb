# required-after-optional params + undef keyword + Kernel.instance_method (rubyspec follow-up)
def f4(s = "", n); [s, n]; end
p f4(1)
p f4(1, 2)
def g(a, b = 2, c); [a, b, c]; end
p g(1, 3)
p g(1, 2, 3)
def h(a = 1, b = 2, c); [a, b, c]; end
p h(5)
p h(5, 6)
p h(5, 6, 7)
p(begin; f4(1, 2, 3); rescue ArgumentError; "argerr"; end)

class UndefA
  def foo; "foo"; end
  def bar; "bar"; end
  undef :bar
  undef foo
end
p(begin; UndefA.new.foo; rescue NoMethodError; "no foo"; end)
p(begin; UndefA.new.bar; rescue NoMethodError; "no bar"; end)

# Kernel.instance_method + define_method on a BasicObject subclass
class BasicProbe < BasicObject
  define_method(:respond_to?, ::Kernel.instance_method(:respond_to?))
  def pub; :pub; end
end
p BasicProbe.new.respond_to?(:pub)
p BasicProbe.new.respond_to?(:nonexistent)
