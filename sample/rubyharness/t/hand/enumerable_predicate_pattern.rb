class C; include Enumerable; def each; yield 1; yield 2; yield 3; end; end
c = C.new
p c.all?(Integer)
p c.all?(String)
p c.any?(2)
p c.none?(String)
p c.one?(2)
p c.all? { |x| x > 0 }
p c.any?
