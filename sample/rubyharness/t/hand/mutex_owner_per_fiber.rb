# Mutex ownership is per fiber (Ruby 3.0+).
m = Mutex.new
p m.synchronize { Fiber.new { m.owned? }.resume }
p m.synchronize { m.owned? }
m.lock
f = Fiber.new { begin; m.lock; rescue ThreadError => e; e.message; end }
p f.resume
f2 = Fiber.new { begin; m.unlock; rescue ThreadError => e; e.class; end }
p f2.resume, m.locked?, m.owned?
m.unlock
p Fiber.new { m.lock; x = m.owned?; m.unlock; x }.resume
q = Mutex.new; t = Thread.new { q.lock; sleep 0.05; q.unlock }; sleep 0.01; p q.owned?, q.locked?; t.join
p m.try_lock, Fiber.new { m.owned? }.resume; m.unlock
