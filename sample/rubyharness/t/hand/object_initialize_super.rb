class C; def initialize(x); @x = x; super(); end; def x; @x; end; end
p C.new(5).x
class D; def initialize; super; end; end
p D.new.class
class Base; def initialize(n); @n = n; end; attr_reader :n; end
class Sub < Base; def initialize(n, m); super(n); @m = m; end; attr_reader :m; end
s = Sub.new(1, 2)
p [s.n, s.m]
class E; def initialize; @v = 10; super(); end; def v; @v; end; end
p E.new.v
begin; Object.new(1); rescue ArgumentError => e; p e.class; end
p Object.new.class
class F; end
p F.new.class
begin; class G; end; G.new(1, 2); rescue ArgumentError; p :argerr; end
class Base2; def initialize; @base = true; end; def base?; @base; end; end
class Mid < Base2; def initialize; super; @mid = true; end; def mid?; @mid; end; end
class Leaf < Mid; def initialize; super; @leaf = true; end; def leaf?; @leaf; end; end
l = Leaf.new
p [l.base?, l.mid?, l.leaf?]
