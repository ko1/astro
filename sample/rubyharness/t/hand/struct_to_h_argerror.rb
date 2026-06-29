S = Struct.new(:a, :b)
s = S.new(1, 2)
p s.to_h { |k, v| [k, v * 10] }
def t; yield; rescue ArgumentError; "AE"; rescue TypeError; "TE"; end
p t { s.to_h { |k, v| [k, v, 99] } }
p t { s.to_h { |k, v| [k] } }
p t { s.to_h { |k, v| :notarray } }
D = Data.define(:x, :y)
d = D.new(1, 2)
p t { d.to_h { |k, v| [k, v, 0] } }
