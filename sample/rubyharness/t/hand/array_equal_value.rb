class V; def initialize(x); @x = x; end; def ==(o); o.is_a?(V) && o.instance_variable_get(:@x) == @x; end; end
p([V.new(1), V.new(2)] == [V.new(1), V.new(2)])
p([1, 2] == [1, 2.0])
p([1, 2] == [1, 3])
p([[1, V.new(5)]] == [[1, V.new(5)]])
p([1, 2, 3] == [1, 2, 3])
p([1, 2] == [1, 2, 3])
p([1, 2] == "x")
p([{ a: V.new(1) }] == [{ a: V.new(1) }])
