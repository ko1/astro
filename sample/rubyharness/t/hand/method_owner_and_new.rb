# `Foo.new` is the default allocator, so reflection must report Class#new — not
# the singleton it was read through, and not Object.  rspec-mocks decides how to
# verify a stubbed `new` with exactly `klass.method(:new) == Class.instance_method(:new).bind(klass)`.
class A1; def initialize(x, y); @x = x; @y = y; end; end
class B1 < A1; end
class C1 < A1; def self.new(_); :redefined; end; end

UM = Class.instance_method(:new)
p [A1.method(:new).owner, UM.owner, UM.bind(A1).owner, A1.method(:new).unbind.owner]
p [C1.method(:new).owner, A1.method(:new).parameters, C1.method(:new).parameters]
p [A1.method(:new) == UM.bind(A1), C1.method(:new) == UM.bind(C1)]

# a `def self.new` added AFTER the capture must not swallow the captured allocator,
# on the class itself or on a subclass the UnboundMethod is rebound to
m = A1.method(:new)
class A1; def self.new(*a); :stub_a; end; end
class B1; def self.new(*a); :stub_b; end; end
p [m.call(1, 2).class, m.unbind.bind(B1).call(3, 4).class, UM.bind(B1).call(5, 6).class]
p [A1.new(1, 2), B1.new(1, 2), m.owner, m.unbind.owner]
p UM.bind_call(B1, 7, 8).class

# an alias of `new` still compares equal to it; a REDEFINED `new` does not
class D1; class << self; alias mk new; end; end
p [D1.method(:mk) == D1.method(:new), D1.method(:mk).owner]

# Method#== must not treat "same receiver + same name" as identity: a superclass
# method rebound onto the same object is a different method (pry walks this chain)
class P2; def f; :p; end; end
class Q2 < P2; def f; :q; end; end
o = Q2.new
p [o.method(:f) == o.method(:f),
   o.method(:f) == P2.instance_method(:f).bind(o),
   o.method(:f) == Q2.instance_method(:f).bind(o),
   o.method(:f) == o.method(:f).super_method,
   o.method(:f).eql?(P2.instance_method(:f).bind(o)),
   o.method(:f).super_method.owner]

# ... and an inherited (not overridden) one still is the same method
class R2 < P2; end
r = R2.new
p [r.method(:f) == P2.instance_method(:f).bind(r), r.method(:f).owner]
