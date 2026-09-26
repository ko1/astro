# Children start with SIGPIPE at its default even under trap("PIPE", "IGNORE"),
# so `yes | head` ends quietly; an uncaught SignalException ends the process
# by that signal (Interrupt still prints), and so does EPIPE on stdout.
def sigign = File.read("/proc/self/status")[/^SigIgn:\s*(\h+)/, 1].to_i(16)
def child(&b)
  r, w = IO.pipe
  pid = fork { r.close; $stdout.reopen(w); $stderr.reopen(w); b.call }
  w.close
  out = r.read; r.close
  Process.wait(pid)
  [out.lines.first&.sub(/\A.*?: /, ""), $?.exitstatus, $?.termsig]
end
p `yes 2>&1 | head -2`
p IO.popen("yes 2>&1 | head -1", &:read)
trap("PIPE", "IGNORE")
p `yes 2>&1 | head -1`
p `sh -c 'grep SigIgn /proc/self/status'`[/\h+$/].to_i(16) & (1 << 12)
trap("PIPE", "DEFAULT")
r, w = IO.pipe; r.close
p((w.write("x") rescue $!.class))
p child { raise Interrupt }
p child { Process.kill(:TERM, $$); sleep 1 }
p child { raise SignalException, "HUP" }
p child { at_exit { exit 3 }; raise SignalException, "USR1" }
p child { trap("TERM", "IGNORE"); raise SignalException, "TERM" }
p child { raise SignalException, "CHLD" }
p child { raise Errno::EPIPE }
p child { begin; raise SignalException, "INT"; rescue SignalException; exit 4; end }
# a stdout that is the pipe from the start (reopen drops the mark in CRuby)
io = IO.popen("-")
if io.nil?
  sleep 0.1; loop { puts "x" * 100 }
end
io.close
p [$?.exitstatus, $?.termsig]
io = IO.popen("-", err: [:child, :out])
if io.nil?
  sleep 0.1
  begin; loop { puts "x" }; rescue Errno::EPIPE => e; $stderr.puts e.instance_variables.inspect; exit! 5; end
end
io.close
p $?.exitstatus
p sigign & (1 << 12)
