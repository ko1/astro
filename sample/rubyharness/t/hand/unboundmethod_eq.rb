class Base; def foo; end; end
class Sub1 < Base; end
class Sub2 < Base; end
p (Sub1.instance_method(:foo) == Sub2.instance_method(:foo))
p (Sub1.instance_method(:foo) == Base.instance_method(:foo))
module M; def bar; end; end
class A; include M; end
class B; include M; end
p (A.instance_method(:bar) == B.instance_method(:bar))
o = Base.new
p (o.method(:foo) == o.method(:foo))
p (Base.new.method(:foo) == Base.new.method(:foo))
