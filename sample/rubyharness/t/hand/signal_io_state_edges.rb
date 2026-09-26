# A signal trapped after another thread saved its context is still handled when
# that thread runs; a failed exec leaves SIGPIPE and the mask as they were; a
# long uncaught message is printed whole; STDOUT.reopen drops the
# "EPIPE ends by SIGPIPE" behaviour; a reopened stdin re-checks what it is.
t = Thread.new { Thread.stop; puts :thread }
sleep 0.05
got = []
trap(:USR1) { got << :handled }   # when it runs differs: CRuby checks in the loop, koruby at the switch
pid = spawn("sh", "-c", "sleep 0.2; kill -USR1 #{$$}")
x = 0; 3_000_000.times { x += 1 }
t.run; t.join
Process.wait(pid)
sleep 0.05
p got
trap(:PIPE, :IGNORE)
ign = -> { File.read("/proc/self/status")[/^SigIgn:\s*(\h+)/, 1].to_i(16)[12] }
before = ign.()
(exec("/bin/true", chdir: "/definitely/missing") rescue nil)
(exec("/definitely/missing/cmd") rescue nil)
p [before, ign.()]
r, w = IO.pipe
pid = fork { r.close; $stderr.reopen(w); raise "x" * 3000 + "\nsecond line" }
w.close; out = r.read; Process.wait(pid)
p out.lines.first.size, out.lines[1]
pid = fork { r, w = IO.pipe; STDOUT.reopen(w); r.close; $stderr.reopen(File::NULL); puts "x" * 70000 }
Process.wait(pid); p [$?.exitstatus, $?.termsig]
File.write("/tmp/koruby_sig_io_edges.txt", "file line\n")
$stdin.reopen("/tmp/koruby_sig_io_edges.txt"); p $stdin.gets
r, w = IO.pipe
$stdin.reopen(r)
Thread.new { sleep 0.2; w.puts "from pipe"; w.close }
p $stdin.gets
File.delete("/tmp/koruby_sig_io_edges.txt")
