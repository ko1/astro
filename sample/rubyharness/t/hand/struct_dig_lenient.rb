# Struct#dig: non-member name digs to nil; no args -> ArgumentError. vs ruby.
k = Struct.new(:a)
i = k.new(k.new({ b: [1, 2, 3] }))
p i.dig(:a, :a)
p i.dig('a', 'a')
p i.dig(:a, :a, :b, 0)
p i.dig(:b, 0)
p i.dig(:nope)
i2 = Struct.new(:a, :b).new(:one, :two)
p i2.dig(0); p i2.dig(1)
begin; i.dig; rescue => e; p e.class; end
begin; i.dig(:a, :a, :b, 0, :too_deep); rescue => e; p e.class; end
