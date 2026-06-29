class C
  include Comparable
  attr_reader :v
  def initialize(v); @v = v; end
  def <=>(o); v <=> o.v; end
  def to_s; "C(#{v})"; end
end
def t; yield; rescue ArgumentError; "AE"; end
a = C.new(5); lo = C.new(1); hi = C.new(10)
p a.clamp(lo, hi).to_s
p C.new(15).clamp(lo, hi).to_s
p C.new(0).clamp(lo, hi).to_s
p t { a.clamp(hi, lo) }
p t { a.clamp(C.new(1)...C.new(10)) }
p a.clamp(C.new(1)..C.new(10)).to_s
