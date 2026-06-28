class V; attr_reader :n; def initialize(n); @n=n; end; def <=>(o); n<=>o.n; end; end
p ([V.new(1)] <=> [V.new(2)])
p ([V.new(2)] <=> [V.new(1)])
p ([V.new(1)] <=> [V.new(1)])
p ([1,2,3] <=> [1,2,4])
p ([1,2,3] <=> [1,2])
p ([1,2] <=> [1,2,3])
p ([1,[2,3]] <=> [1,[2,4]])
p ([Object.new] <=> [Object.new])
class V; attr_reader :n; def initialize(n); @n=n; end; def <=>(o); n<=>o.n; end; end
class W; def to_ary; [1, 2, 3]; end; end
p ([V.new(1)] <=> [V.new(2)])
p ([1,2,3] <=> W.new)
p ([1,2,4] <=> W.new)
p ([1,2,3] <=> 5)
