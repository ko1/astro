class S; def coerce(o); [o, 100]; end; end
p(5 + S.new)
p(5 - S.new)
p(5 * S.new)
p(2.5 + S.new)
p(2.0 * S.new)
class C
  def initialize(n); @n = n; end
  def coerce(o); [C.new(o), self]; end
  def +(o); @n + o.instance_variable_get(:@n); end
  def to_s; "C(#{@n})"; end
end
p((10 + C.new(5)).to_s)
def t; yield; rescue TypeError; "TE"; end
p t { 5 + "x" }
p t { 5 + Object.new }
p(1 + 2)
p(3.0 * 4)
