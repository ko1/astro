D = Data.define(:a, :b)
p D.new(a: 1, b: 2)
p D.new(1, 2)
def t; yield; rescue ArgumentError => e; e.message; end
p t { D.new(a: 1) }
p t { D.new(a: 1, b: 2, c: 3) }
p t { D.new(1) }
p t { D.new }
p t { D.new(1, 2, 3) }
p D[a: 5, b: 6]
