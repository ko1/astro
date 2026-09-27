class V; include Comparable; attr_reader :n; def initialize(n) = @n = n.to_i; def <=>(o) = (o = V.new(o) if o.is_a?(String); n <=> o.n); end
p "1" <=> V.new(2), "3" <=> V.new(2)
p(("1" < V.new(2)), ("3" <= V.new(2)), ("3" > V.new(2)), ("2" >= V.new(2)), ("2" == V.new(2)))
p((("1" < Object.new) rescue $!.class))
