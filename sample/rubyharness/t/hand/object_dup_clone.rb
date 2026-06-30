o = Object.new; o.instance_variable_set(:@x, 5); d = o.dup
p o.equal?(d)
p d.instance_variable_get(:@x)
d.instance_variable_set(:@x, 99); p o.instance_variable_get(:@x)
class Pt; attr_accessor :x, :y; def initialize(x, y); @x, @y = x, y; end; end
pt = Pt.new(1, 2); pt2 = pt.dup
p pt.equal?(pt2)
pt2.x = 99
p pt.x
p pt2.y
c = pt.clone
p pt.equal?(c)
p c.x
p c.y
class WithFreeze; attr_accessor :v; def initialize(v); @v = v; end; end
w = WithFreeze.new([1,2]).freeze
p w.frozen?
p w.dup.frozen?
p w.clone.frozen?
p w.clone(freeze: false).frozen?
e = Object.new
p e.dup.equal?(e)
p e.clone.equal?(e)
pt3 = Pt.new(10, 20)
arr = [pt3.dup, pt3.dup, pt3.dup]
arr.each_with_index { |p, i| p.x = i }
p pt3.x
p arr.map(&:x)
class Shared; attr_accessor :list; def initialize; @list = [1,2,3]; end; end
s = Shared.new; s2 = s.dup
s2.list << 99
p s.list
p s2.list
