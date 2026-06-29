class C; include Enumerable; def each; yield 1; yield 2; yield 3; end; end
c = C.new
p c.select.class
p c.select.to_a
p c.select { |x| x > 1 }
p c.reject.to_a
p c.each_with_index.to_a
p c.group_by.class
p c.partition.class
p c.flat_map.to_a
p c.sort_by.class
p c.find.class
p c.min_by.class
p c.find_index(2)
p c.find_index { |x| x > 1 }
p c.find_index.class
p c.map { |x| x * 2 }
