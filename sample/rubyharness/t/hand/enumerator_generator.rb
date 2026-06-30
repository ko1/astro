e = Enumerator.new { |y| y << 1; y << 2; y << 3 }
p e.to_a
p e.first(2)
p e.take(2)
p e.drop(1)
p e.map { |x| x * 2 }
p e.select(&:odd?)
p e.reduce(:+)
p e.include?(2)
p e.min
p e.max
p e.each_with_index.to_a
inf = Enumerator.new { |y| i = 0; loop { y << (i += 1) } }
p inf.first(5)
p inf.take(3)
fib = Enumerator.new { |y| a, b = 0, 1; loop { y << a; a, b = b, a + b } }
p fib.first(10)
e2 = Enumerator.new { |y| y << 10; y << 20; y << 30 }
p e2.next
p e2.next
p e2.peek
p e2.next
def t; yield; rescue StopIteration; "stop"; end
p t { e2.next }
e2.rewind
p e2.next
p Enumerator.new { |y| [4, 5, 6].each { |x| y << x } }.to_a
