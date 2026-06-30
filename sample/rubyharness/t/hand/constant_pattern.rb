Point = Struct.new(:x, :y)
case Point.new(1, 2); in Point[a, b]; p [a, b]; end
case Point.new(1, 2); in Point(x:, y:); p [x, y]; end
case [Point.new(1,1)]; in [Point[a, b]]; p [a, b]; end
case {pos: Point.new(3, 4)}; in {pos: Point[x, y]}; p [x, y]; end
case Point.new(5, 6); in Point[5, b]; p "x=5, y=#{b}"; end
case "hello"; in Point[a, b]; p "is point"; else; p "not point"; end
case [Point.new(1,1), Point.new(2,2)]; in [*, Point[2, y], *]; p y; end
Coord = Data.define(:lat, :lng)
case Coord.new(1.0, 2.0); in Coord(lat:, lng:); p [lat, lng]; end
case Point.new(1, 2); in Point[Integer => a, Integer => b]; p [:typed, a, b]; end
case Point.new(1, 2); in Point[a, *rest]; p [a, rest]; end
def shape(s); case s; in Point[0, 0]; :origin; in Point[x, 0]; [:x_axis, x]; in Point[0, y]; [:y_axis, y]; in Point[x, y]; [:point, x, y]; end; end
p shape(Point.new(0, 0))
p shape(Point.new(5, 0))
p shape(Point.new(3, 4))
case Coord.new(10.0, 20.0); in Coord[lat, lng]; p [lat, lng]; end
