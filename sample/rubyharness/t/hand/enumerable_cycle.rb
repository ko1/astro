p (1..3).cycle.first(7)
p (1..3).cycle(2).to_a
p (1..3).cycle.take(8)
p ('a'..'c').cycle.first(5)
r = []; (1..3).cycle(2) { |x| r << x }; p r
cnt = 0; (1..3).cycle { |x| cnt += 1; break if cnt >= 7 }; p cnt
p (1..3).cycle { |x| break x if x == 2 }
class Stk; include Enumerable; def initialize(*i); @i=i; end; def each; @i.each { |x| yield x }; end; end
p Stk.new(1,2).cycle.first(5)
p Stk.new(1,2,3).cycle(2).to_a
c2 = 0; Stk.new(1,2).cycle { c2 += 1; break if c2 >= 5 }; p c2
p [].cycle.first(3)
p (1..3).cycle.first(0)
p (1..3).cycle(2).map { |x| x * 10 }
p (1..3).cycle.lazy.select(&:odd?).first(4)
