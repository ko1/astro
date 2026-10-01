# attr_writer / Struct setters called through a cached site go straight to the ivar store:
# return value, new ivars (shape change), frozen receivers, private writers, redefinition,
# subclasses and send behave as CRuby.  Run under both and diff.
S = Struct.new(:a, :b)
s = S.new(1, 2)
r = (s.b = 5)
p r, s.b, s.to_a
3.times { |i| s.a = i }
p s.a
class W
  attr_writer :x, :y
  attr_reader :x, :y
  def set_y(v) = (self.y = v)
end
w = W.new
p w.x, (w.x = 1), w.x
[W.new, W.new].each_with_index { |o, i| o.y = i; p o.y }
class W2 < W; end
w2 = W2.new; w2.x = :sub; p w2.x
w.freeze
begin; w.x = 2; rescue => e; p e.class, e.message =~ /frozen/ ? :frozen : e.message; end
begin; s.freeze; s.a = 9; rescue => e; p e.class; end
class P
  attr_writer :z
  private :z=
  def setz(v) = (self.z = v)
  def z = @z
end
q = P.new
begin; q.z = 1; rescue NoMethodError => e; p e.message[/private method/]; end
q.setz(3); p q.z
p q.send(:z=, 4), q.z
o = W.new
5.times { o.x = 1 }
class W; def x=(v); @x = v * 10; end; end
o.x = 2; p o.x
p W.instance_method(:y=).arity, S.instance_method(:a=).arity
def o.y=(v); @y = :singleton; end
o.y = 1; p o.y
# statements whose value is discarded (an ordinary send, no temps)
class P
  def setz2(v)
    self.z = v
    :done
  end
end
p q.setz2(7), q.z
n = nil
log = []
n&.x = (log << :rhs)
o2 = W.new
o2.x = (log << :eval; 1)
p log, o2.x
w3 = W.new; w4 = W.new
w3.x = (w4.y = 6)
p w3.x, w4.y
order = []
(order << :recv; w3).y = (order << :rhs; 8)
p order, w3.y
