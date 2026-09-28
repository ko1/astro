f = Fiber.new { |a, b| x = Fiber.yield(a + b); [x, :end] }
p f.resume(1, 2), f.resume(:v), f.alive?
g = Fiber.new { |*a| a }; p g.resume(1, 2, 3)
h = Fiber.new { |a| a }; p h.resume([1, 2])
pr = proc { |v| v * 2 }; p Fiber.new(&pr).resume(21)
l = ->(v) { v + 1 }; p Fiber.new(&l).resume(1)
p((Fiber.new { break 1 }.resume rescue $!.class))
def m = Fiber.new { return 1 }.resume
p((m rescue $!.class))
e = Fiber.new { raise "boom" }; p((begin; e.resume; rescue => x; x.message; end), e.alive?)
p Fiber.new(blocking: true) { Fiber.current.blocking? }.resume
p Fiber.new { self }.resume.inspect == self.inspect
def mk(n) = Fiber.new { n * 10 }
fs = (1..3).map { |i| mk(i) }; GC.start; p fs.map(&:resume)
en = Enumerator.new { |y| y << 1; y << 2 }; p en.next, en.next
p Fiber.new { Fiber.new { :inner }.resume }.resume
f2 = Fiber.new { |*a| b = Fiber.yield(*a, :y); c = Fiber.yield; [b, c] }
p f2.resume(1, 2), f2.resume(3, 4), f2.resume, Fiber.new { Fiber.yield(1, 2) }.resume, Fiber.new { Fiber.yield }.resume
t = Fiber.new { |a, b| [a, b] }; p t.transfer(5, 6)
