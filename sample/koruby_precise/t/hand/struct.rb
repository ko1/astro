# L1: Struct, keyword_init, methods on struct
Point = Struct.new(:x, :y)
pt = Point.new(1, 2)
p pt.x
p pt.y
p pt.to_a
p pt.to_h
p pt.members
pt.x = 10
p pt.x
p Point.new(1, 2) == Point.new(1, 2)
p pt == Point.new(1, 2)

Point3 = Struct.new(:x, :y, :z) do
  def norm2
    x * x + y * y + z * z
  end
end
p Point3.new(1, 2, 2).norm2

KV = Struct.new(:key, :value, keyword_init: true)
kv = KV.new(key: "a", value: 1)
p kv.key
p kv.value
p kv.to_h

Rec = Struct.new(:a, :b)
r = Rec.new(1, 2)
p r[0]
p r[:a]
p r.each.to_a
p r.map { |v| v * 10 }
a, b = *Rec.new(3, 4)
p [a, b]
p Rec.members
