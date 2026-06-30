S = Struct.new(:a, :b)
p S.new(1, 2).is_a?(Struct)
p S.ancestors.include?(Struct)
p S.superclass
p S.new(1, 2).to_a
p S.new(1, 2) == S.new(1, 2)
p S.new(3, 4).map { |v| v * 2 }
p S.new(1, 2).members
D = Data.define(:a, :b)
p D.new(1, 2).is_a?(Data)
p D.ancestors.include?(Data)
p D.superclass
p D.new(1, 2).to_h
p D.new(1, 2).with(a: 10).to_h
p Struct.new("Named", :x).new(5).is_a?(Struct)
