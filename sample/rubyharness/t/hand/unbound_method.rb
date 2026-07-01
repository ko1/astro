# Module#instance_method returns a real UnboundMethod (distinct class), which
# rebinds to a compatible instance. vs ruby.
class A
  def foo; "A#foo"; end
  def val; @v; end
end
class B < A
  def foo; "B#foo"; end
end

um = A.instance_method(:foo)
p um.class
p um.class.name
p defined?(UnboundMethod)
p UnboundMethod.superclass
p um.name
p um.owner
p um.arity

# bind to an instance of the owner (or subclass) and invoke the FIXED method
p um.bind(A.new).call
p um.bind(B.new).call          # still A#foo — unbound methods are not virtual
p um.bind_call(A.new)

# Method#unbind round-trips
m = A.new.method(:foo)
p m.unbind.class
p m.unbind.bind(A.new).call

# equality: two unbound methods for the same owner+name are equal
p A.instance_method(:foo) == A.instance_method(:foo)
p A.instance_method(:foo) == B.instance_method(:foo)

# bind rejects an incompatible receiver
begin
  A.instance_method(:foo).bind("string")
rescue TypeError => e
  p e.class
end
