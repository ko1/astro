class U; end
class << U; private :new; end
begin; U.new; p :no_raise; rescue NoMethodError => e; p e.message; end
class V; private_class_method :new; end
begin; V.new; p :no_raise; rescue NoMethodError => e; p e.message; end
class W; class << self; private :allocate; end; end
p U.send(:new).class
class Dog2; def initialize(a = 1); end; end
m = Module.new { def new(*a, **k, &b) = super }
Dog2.singleton_class.prepend(m)
x = Dog2.method(:new)
y = Dog2.method(:new).super_method
p y&.call&.class
p Class.new.method(:new).owner, Dog2.singleton_class.ancestors[1, 3]
o = Object.new
m2 = Module.new { def to_s = "p" }
o.singleton_class.prepend(m2)
x = Dog2.method(:new); p x.owner == m, x.super_method.owner, x.super_method.call.class
