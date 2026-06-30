Point = Struct.new(:x, :y)
p [Point.new(1, 2), Point.new(3, 4)]
p({a: Point.new(5, 6)})
p [Point.new(1, [2, 3])]
Coord = Data.define(:lat, :lng)
p [Coord.new(1.5, 2.5)]
p({loc: Coord.new(1.0, 2.0)})
Nested = Struct.new(:inner)
p Nested.new(Point.new(7, 8))
p [Point.new(1,2)].to_s
p Point.new(1, 2).inspect
puts [Point.new(1,1), Point.new(2,2)].inspect
require 'set'
p Set[Point.new(1,2)]
p [[Point.new(1,2)]]
p Point.new(1, 2)
p [1, Point.new(2,3), "x"]
p({list: [Point.new(1,1)], count: 1})
