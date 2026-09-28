# format("%s"/"%p") of a Thread / Fiber dispatches #to_s / #inspect; inspect says sleep_forever for untimed waits
t = Thread.current
p format("%s", t) == t.to_s, format("%p", t) == t.inspect
f = Fiber.new {}; p format("%s", f) == f.to_s
m = Mutex.new; m.lock
ts = [Thread.new { Thread.stop }, Thread.new { Queue.new.pop }, Thread.new { sleep }, Thread.new { sleep 10 },
      Thread.new { m.lock }, Thread.new { Queue.new.pop(timeout: 10) }, Thread.new { SizedQueue.new(1).tap { _1 << 1 } << 2 }]
sleep 0.3
ts.each { |x| p [x.inspect[/ (\w+)>\z/, 1], x.status] }
ts.each(&:kill)
