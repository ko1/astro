Car = Struct.new(:make, :model, :year)
c = Car.new("Ford", "Ranger", nil)
p c.to_s
p c.inspect
puts c
anon = Struct.new(:a).new("")
p anon.to_s
D = Data.define(:x, :y)
p D.new(1, 2).inspect
