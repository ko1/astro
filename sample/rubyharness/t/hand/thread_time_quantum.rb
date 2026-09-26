# A thread keeps the CPU for its quantum (CRuby: 100ms): a new thread does not
# run until its creator blocks or the slice ends, not after a few loop turns.
# simplecov's production flusher raced on that (a 600s wait nobody signalled).
x = []
t = Thread.new { x << :child }
x << :main1
1000.times { |i| i }
i = 0; i += 1 while i < 10_000
x << :main2
t.join
m = Mutex.new; cv = ConditionVariable.new; r = true
t2 = Thread.new { m.synchronize { cv.wait(m, 5) while r }; x << :t2done }
r = false
t2.join(3)
p x, t2.status
spun = false
t3 = Thread.new { spun = true }
t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
nil until spun                                   # a busy loop still hands over after the quantum
p spun, Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0 < 2
