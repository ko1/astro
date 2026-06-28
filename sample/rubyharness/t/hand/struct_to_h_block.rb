S = Struct.new(:make, :model, :year)
s = S.new("Ford", "Ranger", nil)
p s.to_h
p s.to_h { |k, v| [k.to_s, v.to_s] }
out = []
s.to_h { |k, v| out << [k, v]; [k, v] }
p out
D = Data.define(:x, :y)
p D.new(1, 2).to_h { |k, v| [k, v * 10] }
