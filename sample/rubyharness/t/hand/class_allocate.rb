class C; def initialize; @x = 1; end; def x; @x; end; end
o = C.allocate
p o.class
p o.x
p o.instance_of?(C)
o2 = C.new
p o2.x
class D; def initialize(a); @a = a; end; end
p D.allocate.class
p C.allocate.is_a?(C)
