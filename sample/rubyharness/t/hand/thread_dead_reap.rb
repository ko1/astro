t = Thread.new { sleep }; Thread.pass; t.kill; p t.join.equal?(t), t.alive?
u = Thread.new { 1 }; u.kill; p u.value, u.status
q = Queue.new; ths = 5.times.map { |i| Thread.new { q << i; Thread.current[:x] = i; i * 2 } }; p ths.map(&:value), q.size
m = Mutex.new; th = Thread.new { m.lock; :locked }; p th.value, m.locked?, m.owned?
e = Thread.new { raise "x" }; e.report_on_exception = false rescue nil; p((e.value rescue $!.message))
p Thread.list.size
# many short threads reuse the pooled stacks of dead ones
r = 2000.times.map { |i| Array.new(2) { |j| Thread.new { a = [i, j] * 20; Thread.pass; a.sum } }.map(&:value).sum }
p r.sum
