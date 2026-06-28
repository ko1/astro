class C; include Enumerable; def each; yield 1; yield 2; yield 3; end; end
c = C.new
out = []; c.cycle(2) { |x| out << x }; p out
p c.one? { |x| x == 2 }
p c.tally
p c.partition { |x| x.odd? }
p c.group_by { |x| x % 2 }
p c.each_with_object([]) { |x, o| o << x * 10 }
p c.take(2)
p c.drop(1)
p c.minmax
p c.find_index(2)
