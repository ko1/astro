class N
  attr_reader :v
  def initialize(v); @v = v; end
  def coerce(o); [N.new(o), self]; end
  def +(o); N.new(v + (o.is_a?(N) ? o.v : o)); end
  def <=>(o); v <=> (o.is_a?(N) ? o.v : o); end
  def to_s; "N(#{v})"; end
end
p (1 + N.new(2)).to_s
class R; def coerce(o); [2.0, 5.0]; end; end
p 1 + R.new, 1 - R.new, 1 * R.new, 1.0 / R.new, 7.div(R.new), 7.divmod(R.new)
