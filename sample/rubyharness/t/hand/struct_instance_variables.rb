S = Struct.new(:make, :model, :year)
s = S.new("Ford", "Ranger", nil)
p s.instance_variables
s.instance_variable_set(:@extra, 1)
p s.instance_variables
D = Data.define(:x)
p D.new(5).instance_variables
