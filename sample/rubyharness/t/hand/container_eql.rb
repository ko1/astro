class V; def initialize(x); @x = x; end; def eql?(o); o.is_a?(V) && o.instance_variable_get(:@x) == @x; end; def hash; @x.hash; end; end
p([V.new(1)].eql?([V.new(1)]))
p({ a: V.new(1) }.eql?({ a: V.new(1) }))
p([1, 2].eql?([1, 2]))
p([1].eql?([1.0]))
p([[V.new(2)]].eql?([[V.new(2)]]))
p({ a: 1 }.eql?({ a: 1 }))
p({ a: 1 }.eql?({ a: 1.0 }))
p([1, 2].eql?("x"))
