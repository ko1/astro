# Method#unbind keeps the definition the Method captured; a later redefinition
# of the name must not leak into the UnboundMethod (rspec-mocks' and_call_original
# on a subclass whose superclass method is stubbed).
A = Class.new { def self.foo; "orig"; end }
B = Class.new(A)
m = A.method(:foo)
A.singleton_class.send(:undef_method, :foo)
A.singleton_class.send(:define_method, :foo) { :fake }
p m.call
um = m.unbind
p um.bind(A).call, um.bind(B).call, um.bind_call(B), um.name, um.owner == A.singleton_class
m2 = A.method(:foo); A.singleton_class.send(:define_method, :foo) { :fake2 }
p m2.unbind.bind(A).call, A.foo, B.foo
class C; def foo; "orig"; end; end
m3 = C.new.method(:foo); class C; def foo; :new; end; end
p m3.unbind.bind(C.new).call, C.new.foo, m3.call
