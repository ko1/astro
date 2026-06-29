S = Struct.new(:a)
i1 = S.new(S.new({ b: [1, 2, 3] }))
p i1.dig(:a, :a)
i2 = S.new(1)
def t; yield; rescue TypeError; "TE"; rescue => e; e.class.to_s; end
p t { i2.dig(:a, 3) }
H = Struct.new(:x, :y)
h = H.new({ k: 5 }, nil)
p h.dig(:x, :k)
