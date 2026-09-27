e = [1, 2, 3].cycle
p 7.times.map { e.next }, e.peek, e.next
e.rewind; p e.next
a = [:x, :y]; c = a.cycle; p c.next; a << :z; p [c.next, c.next, c.next]
p [].cycle.size, ([].cycle.next rescue $!.class)
l = [1, 2].cycle.lazy.map { _1 * 10 }; p l.first(3)
s = 0; big = [1, 2, 3].cycle; 30000.times { s += big.next }; p s
p [1,2].cycle.each_with_index.first(3), [1,2].cycle.with_index(5).first(2), [1,2].cycle.with_index.size
r = []; [1,2].cycle.with_index { |x, i| r << [x, i]; break if i >= 2 }; p r
r = []; [1,2].cycle.each { |x| r << x; break if r.size > 3 }; p r
e = [1,2].cycle.each_with_index; p e.next, e.next, e.next
p [1,2].cycle.lazy.select(&:odd?).first(2), [1,2].cycle.take(5), [1, 2].cycle.lazy.take(3).to_a
