S = Struct.new(:a, :b, :c)
s = S.new(1, 2, 3)
p s.select { |x| x > 1 }
p s.map { |x| x * 2 }
p s.to_a
p s.find { |x| x == 2 }
p s.include?(2)
p s.reduce(:+)
p s.min
p s.max
p S.ancestors.include?(Enumerable)
p s.partition { |x| x.even? }
p s.sort
p s.count { |x| x > 1 }
