# Signals are still delivered at check points once something is trapped (no reap before any trap), Process.pid is
# cached but follows fork, and kill to ourselves raises / runs the trap as before
p Process.pid == $$, Process.pid.is_a?(Integer)
m = Mutex.new; q = Queue.new
3.times { m.synchronize { q << 1 } }
p q.size
begin
  Process.kill(:TERM, Process.pid)
  p :not_reached
rescue SignalException => e
  p e.class, e.signo
end
got = []
old = trap(:USR1) { got << :usr1 }
Process.kill(:USR1, Process.pid)
m.synchronize { }
sleep 0.01
p got
trap(:USR1, old || "DEFAULT")
r, w = IO.pipe
pid = fork do
  r.close
  w.write [Process.pid == $$, Process.pid != Process.ppid].inspect
  w.close
end
w.close
p r.read, Process.wait(pid) == pid
th = Thread.new { m.synchronize { 7 } }
p th.value, Process.pid == $$
