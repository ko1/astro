def t; yield; rescue => e; e.class; end
p t { Struct.new(:a, :a) }
p t { Struct.new(:a, 1) }
p t { Struct.new(:a, b: 2) }
S = Struct.new(:x, :y)
p S.new(1, 2).to_a
S2 = Struct.new(:a, keyword_init: true)
p S2.new(a: 5).a
