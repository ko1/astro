# Method#super_method returns the next definition up the MRO (fixed owner). vs ruby.
module M1; def foo; "M1+" + (defined?(super) ? super : ""); end; end
class A; def foo; "A"; end; end
class B < A; include M1; def foo; "B+" + super; end; end
m = B.new.method(:foo)
p m.call
sm = m.super_method
p sm.owner
p sm.call
p sm.super_method.owner
p sm.super_method.call
p sm.super_method.super_method
p A.new.method(:foo).super_method
p B.instance_method(:foo).super_method.owner
