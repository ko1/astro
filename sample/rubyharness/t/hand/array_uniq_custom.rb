class Pt
  attr_reader :x, :y
  def initialize(x, y); @x, @y = x, y; end
  def ==(o); o.is_a?(Pt) && x == o.x && y == o.y; end
  alias eql? ==
  def hash; [x, y].hash; end
end
p [Pt.new(1,2), Pt.new(1,2), Pt.new(3,4)].uniq.size
p [Pt.new(1,2), Pt.new(1,2), Pt.new(3,4)].uniq.map { |pt| [pt.x, pt.y] }
arr = [Pt.new(1,2), Pt.new(1,2), Pt.new(3,4)]
r = arr.uniq!
p arr.size
p r.nil?
arr2 = [Pt.new(1,2), Pt.new(3,4)]
p arr2.uniq!.nil?
p [Pt.new(1,2), Pt.new(1,3), Pt.new(2,9)].uniq { |pt| pt.x }.size
p [1, 2, 2, 3, 3, 3].uniq
p ["a", "a", "b"].uniq
p [1, 1.0, 1].uniq
p [:a, :a, :b].uniq.size
