class Pt
  attr_reader :x, :y
  def initialize(x, y); @x, @y = x, y; end
  def ==(o); o.is_a?(Pt) && x == o.x && y == o.y; end
  alias eql? ==
  def hash; [x, y].hash; end
end
h = {}
h[Pt.new(1, 2)] = "a"
h[Pt.new(3, 4)] = "b"
p h[Pt.new(1, 2)]
p h[Pt.new(3, 4)]
p h[Pt.new(9, 9)]
h[Pt.new(1, 2)] = "updated"
p h.size
p h[Pt.new(1, 2)]
p h.key?(Pt.new(3, 4))
p h.key?(Pt.new(9, 9))
p h.fetch(Pt.new(1, 2))
p h.fetch(Pt.new(9, 9), "def")
h.delete(Pt.new(3, 4))
p h.size
p h.key?(Pt.new(3, 4))
lit = {Pt.new(1,2) => 1, Pt.new(1,2) => 2}
p lit.size
p lit.values
counts = Hash.new(0)
[Pt.new(1,1), Pt.new(1,1), Pt.new(2,2)].each { |pt| counts[pt] += 1 }
p counts.values.sort
m = {Pt.new(1,2) => 1}.merge(Pt.new(1,2) => 2)
p m.size
p m[Pt.new(1,2)]
