# A sleeping thread must wake on time while two or more other threads keep
# yielding (Thread.pass) or spinning — the run queue never drains then.
t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
elapsed = -> { Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0 }
[[2, :pass], [4, :pass], [3, :busy]].each do |n, mode|
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  stop = false
  spin = Array.new(n) { Thread.new { c = 0; until stop || elapsed.() > 3; c += 1; Thread.pass if mode == :pass; end } }
  w = Thread.new { sleep 0.1; elapsed.() }
  woke = w.value
  stop = true
  spin.each(&:join)
  p [n, mode, woke < 1.0]
end
q = Queue.new
spin = Array.new(2) { Thread.new { Thread.pass until q.size > 0 } }
r = Thread.new { sleep 0.05; q << :go }
spin.each(&:join); r.join
p q.pop
