# Integer comparison (<,>,<=,>=) and bit ops (&,|,^) use the #coerce protocol
# for a coercible user object; Float/non-coercible raise. vs ruby.
class CN
  def initialize(v); @v = v; end
  def coerce(o); [CN.new(o), self]; end
  def <(o); @v < o.v; end
  def >(o); @v > o.v; end
  def <=(o); @v <= o.v; end
  def >=(o); @v >= o.v; end
  attr_reader :v
end
big = 10**40
p(big < CN.new(big * 2))
p(big < CN.new(1))
p(5 > CN.new(3))
p(5 <= CN.new(5))
class BC; def coerce(o); [6, 3]; end; end
p(6 & BC.new)
p(6 | BC.new)
p(6 ^ BC.new)
begin; 5 & 1.5; rescue => e; p e.class; end
begin; 5 & Object.new; rescue => e; p e.class; end
begin; 5 > "x"; rescue => e; p e.class; end
begin; 5 > Object.new; rescue => e; p e.class; end
