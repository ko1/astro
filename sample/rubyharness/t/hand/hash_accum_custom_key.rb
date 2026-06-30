class Pt
  attr_reader :x, :y
  def initialize(x, y); @x, @y = x, y; end
  def ==(o); o.is_a?(Pt) && x == o.x && y == o.y; end
  alias eql? ==
  def hash; [x, y].hash; end
end
pts = [Pt.new(1,1), Pt.new(1,1), Pt.new(2,2), Pt.new(1,1)]
g = pts.group_by { |pt| pt }
p g.size
p g.values.map(&:size).sort
p pts.tally.values.sort
h = {Pt.new(1,2) => "a", Pt.new(3,4) => "b"}
p h.values_at(Pt.new(1,2), Pt.new(3,4))
p h.values_at(Pt.new(9,9))
p [1,2,3,4,5].group_by(&:even?)
p [1,1,2,3,3,3].tally
p({a: 1, b: 2}.values_at(:a, :b))
p pts.group_by(&:x).transform_values(&:size)
