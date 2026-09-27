e = (1..).lazy.map { _1 * 2 }
p 3.times.map { e.next }, e.peek, e.next_values, e.peek_values
e.rewind; p e.next
f = [1, 2, 3].lazy.select(&:odd?); p f.next, f.next, (f.next rescue $!.class)
g = (1..3).lazy.map { |x| x }; p g.next, g.next, g.next, (g.next rescue [$!.class, $!.result])
h = (1..Float::INFINITY).lazy.filter_map { _1 * 3 if _1.even? }.take(2); p h.next, h.next, (h.next rescue $!.class)
s = 0; big = (1..).lazy.map { _1 + 1 }; 20000.times { s += big.next }; p s
a = (1..3).lazy.map { _1 }.each { }
b = [1, 2].lazy.select { true }.each { }
c = (1..3).lazy.each { }
p a, b, c
