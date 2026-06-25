# Proc / Fiber subclassing + Proc.new(&proc/&method) (rubyspec follow-up)
class MyFiber < Fiber; end
f = MyFiber.new { Fiber.yield 10; 20 }
p f.resume
p f.resume
p f.instance_of?(MyFiber)

sub = Class.new(Proc)
pr = sub.new { "ok" }
p pr.call
p pr.is_a?(Proc)

# Proc.new(&proc) returns the proc; Proc.new(&method) builds one
base = proc { |x| x + 1 }
fwd = Proc.new(&base)
p fwd.equal?(base)
p fwd.call(10)
m = "hello".method(:size)
mp = Proc.new(&m)
p mp.call
