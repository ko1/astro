# Comparable operators derive from #<=> using rb_cmpint semantics: the result
# is compared by SIGN, so a Float or any object (via >/< 0) works, not only an
# Integer. vs ruby.
class W
  include Comparable
  def initialize(r); @r = r; end
  def <=>(o); @r; end
end
p(W.new(1) > W.new(0))
p(W.new(0.1) > W.new(0))
p(W.new(0) > W.new(0))
p(W.new(-1.0) > W.new(0))
p(W.new(10000000) >= W.new(0))
p(W.new(-0.0001) < W.new(0))
p(W.new(0.0) <= W.new(0))
p(W.new(5) == W.new(0))
p(W.new(0) == W.new(0))
begin
  W.new(nil) > W.new(0)
rescue ArgumentError => e
  p e.class
end
