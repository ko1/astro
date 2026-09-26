# Process.wait / wait2 / system / popen#close park the waiting green thread, so
# another thread keeps running while the child lives (a blocking waitpid froze
# them all: a DRb server thread starved the forked client it was serving).
require 'socket'
r = []
t = Thread.new { 3.times { |i| r << i; sleep 0.05 } }
pid = fork { sleep 0.3; exit!(7) }
Process.wait(pid); p r, $?.exitstatus
p Process.wait2(fork { exit!(3) })[1].exitstatus
fork { sleep 0.1; exit!(4) }
p Process.wait.is_a?(Integer), $?.exitstatus
p((Process.wait rescue $!.class))
p system("exit 2"), $?.exitstatus
q = []
t2 = Thread.new { 3.times { |i| q << i; sleep 0.05 } }
system("sleep 0.3"); p q
srv = TCPServer.new("127.0.0.1", 0); port = srv.addr[1]
th = Thread.new { s = srv.accept; l = s.gets; s.puts "echo #{l}"; s.close }
pid = fork { c = TCPSocket.new("127.0.0.1", port); c.puts "x"; $stdout.sync = true; puts c.gets; exit!(0) }
Process.wait(pid); p $?.exitstatus; th.join
q2 = []
t4 = Thread.new { 3.times { |i| q2 << i; sleep 0.05 } }
p `sleep 0.3; echo bt`, q2                         # backtick reads and reaps without stalling t4
