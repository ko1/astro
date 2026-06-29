class W; include Comparable; def <=>(o); 99; end; end
a = W.new
def a.<=>(o); 0; end
p(a == W.new)
