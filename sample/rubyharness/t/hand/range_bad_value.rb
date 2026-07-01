# Range.new / literal ranges with two non-nil, incomparable bounds raise
# ArgumentError 'bad value for range' (CRuby). Comparable bounds are accepted.
# vs ruby.
begin; Range.new("a", 1); rescue => e; p e.class; p e.message; end
begin; Range.new(1, "z"); rescue => e; p e.class; end
begin; Range.new(Object.new, Object.new); rescue => e; p e.class; end
begin; ("a"..5); rescue => e; p e.class; end
# valid ranges still build (numeric / string / symbol / nil-bounded / custom)
p (1..10).to_a
p ("a".."e").to_a
p (1.5..3.5).to_a rescue p "float-range-to_a"
p (nil..5).end
p (1..).begin
class C
  include Comparable
  attr_reader :v
  def initialize(v); @v = v; end
  def <=>(o); v <=> o.v; end
end
r = C.new(1)..C.new(9)
p r.begin.v
p r.end.v
