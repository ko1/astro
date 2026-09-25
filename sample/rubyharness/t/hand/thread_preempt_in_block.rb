# A thread spinning inside a C-driven iteration (Integer#times, loop, each) must
# still let the others run: green threads are only preempted at yield points.
stop = false
a = Thread.new { 1_000_000_000.times { |i| break if stop } }
b = Thread.new { 10.times.map { Thread.pass; 1 } }
p b.value.sum
stop = true; a.join
c = Thread.new { loop { break if stop == :x } }
d = Thread.new { [1, 2, 3].each { |x| x }; :d }
p d.value; stop = :x; c.join
e = Thread.new { (1..Float::INFINITY).each { |i| break i if stop == :y } }
f = Thread.new { sleep 0.01; stop = :y }
p e.value.is_a?(Integer); f.join
