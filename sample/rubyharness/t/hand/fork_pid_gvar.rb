# $$ in a forked child (fork / fork {} / IO.popen("-")) is the child's own pid;
# libraries detect a fork by comparing it (concurrent-ruby's pools).
parent = $$
pid = fork { p [$$ == Process.pid, $$ != parent, Process.ppid == parent]; exit!(0) }
Process.wait(pid)
t = Thread.new { sleep 0.01; :ok }
pid = fork { t2 = Thread.new { 1 + 1 }; p t2.value; q = Queue.new; Thread.new { q << :x }; p q.pop; exit!(0) }
Process.wait(pid); p $?.exitstatus
io = IO.popen("-")
if io.nil?
  puts [$$ == Process.pid, $$ != parent].inspect
  exit!(0)
end
p io.read; io.close
pid = fork
exit!($$ == Process.pid ? 3 : 4) if pid.nil?
Process.wait(pid); p $?.exitstatus
p $$ == parent, t.value
