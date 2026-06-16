# Struct.new — core: class creation, positional init, accessors (oracle = CRuby).
# (optcarrot's opt.rb does `MethodDef = Struct.new(:params, :body)`.)
Point = Struct.new(:x, :y)
p Point.class
a = Point.new(3, 4)
p a.x
p a.y
a.x = 10
p a.x
b = Point.new(1)        # missing trailing arg → nil
p b.y
c = Point.new           # all nil
p [c.x, c.y]
p a.is_a?(Point)
Pair = Struct.new(:a, :b)
pr = Pair.new([1, 2], "hi")
p pr.a
p pr.b
