# Struct.new(...).new with too many positional args raises ArgumentError
# "struct size differs". vs ruby.
S = Struct.new(:a, :b)
begin; S.new(1, 2, 3); rescue ArgumentError => e; p e.message; end
begin; S.new(1, 2, 3, 4); rescue ArgumentError => e; p e.message; end
p S.new(1, 2).to_a
p S.new(1).to_a
p S.new.to_a
K = Struct.new(:x, keyword_init: true)
p K.new(x: 9).x
