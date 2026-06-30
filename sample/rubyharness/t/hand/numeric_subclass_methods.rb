# Generic Numeric methods inherited by a Numeric subclass (div/divmod/modulo/
# remainder/zero?/nonzero?/negative?/positive?/integer?/to_int/fdiv). vs ruby.
class N < Numeric
  attr_reader :v
  def initialize(v); @v = v; end
  def <(o); @v < (o.is_a?(N) ? o.v : o); end
  def >(o); @v > (o.is_a?(N) ? o.v : o); end
  def ==(o); @v == (o.is_a?(N) ? o.v : o); end
  def coerce(o); [N.new(o), self]; end
  def -@; N.new(-@v); end
  def +(o); N.new(@v + (o.is_a?(N) ? o.v : o)); end
  def -(o); N.new(@v - (o.is_a?(N) ? o.v : o)); end
  def *(o); N.new(@v * (o.is_a?(N) ? o.v : o)); end
  def /(o); N.new(@v / (o.is_a?(N) ? o.v : o)); end
  def floor; N.new(@v.floor); end
  def to_i; @v.to_i; end
  def to_f; @v.to_f; end
  def to_s; "N(#{@v})"; end
end
p N.new(7).zero?; p N.new(0).zero?; p N.new(0).nonzero?; p N.new(5).nonzero?.to_s
p N.new(-3).negative?; p N.new(3).positive?; p N.new(7).integer?; p N.new(7).to_int
p N.new(7).div(2).to_s; p N.new(7).divmod(2).map(&:to_s); p N.new(7).modulo(3).to_s
p N.new(7).fdiv(2)
# concrete numerics keep their own behavior
p (-7).abs; p 7.div(2); p 7.divmod(3); p 7.remainder(3); p (-7).remainder(3); p 7.fdiv(2); p 7.0.zero?
