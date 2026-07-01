# Struct#size / #length return the member count. vs ruby.
S = Struct.new(:a, :b, :c)
p S.new(1, 2, 3).size
p S.new(1, 2, 3).length
p Struct.new(:x).new(5).size
p Struct.new(:a, :b, :c, :d, :e).new.size
p S.new(1, 2, 3).to_a.length == S.new(1, 2, 3).size
