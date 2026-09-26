# An untrapped HUP/INT/QUIT/TERM/ALRM/USR1/USR2 from outside raises in the main
# thread while it waits (sleep, Process.wait, join, Queue#pop, a pipe read, a
# fiber), so rescue / ensure / at_exit run; an inherited ignore stays ignored.
def run(sig, &blk)
  r, w = IO.pipe
  rd, wr = IO.pipe
  pid = fork do
    r.close; rd.close
    $stdout.reopen(w)
    begin
      blk.call(wr)
    rescue SignalException => e
      puts "rescued #{e.class} #{e.signm.inspect} main=#{Thread.current == Thread.main}"
    ensure
      puts "ensure"
    end
    exit!(0)
  end
  w.close; wr.close
  rd.read(1)                 # the child is about to block
  sleep 0.2
  Process.kill(sig, pid)
  out = r.read
  Process.wait(pid)
  [sig, out.split("\n"), $?.exitstatus]
end
ready = ->(wr) { wr.write("x"); wr.close }
p run(:INT)  { |wr| ready[wr]; sleep 5 }
p run(:TERM) { |wr| ready[wr]; pid = spawn("sleep 5"); begin; Process.wait(pid); ensure; Process.kill(:KILL, pid); end }
p run(:HUP)  { |wr| t = Thread.new { sleep 5 }; ready[wr]; t.join }
p run(:USR1) { |wr| q = Queue.new; Thread.new { sleep 5 }; ready[wr]; q.pop }
p run(:USR2) { |wr| i, o = IO.pipe; ready[wr]; i.read }
p run(:QUIT) { |wr| f = Fiber.new { ready[wr]; sleep 5 }; f.resume }
p run(:ALRM) { |wr| trap(:ALRM) { puts "trap"; raise "from trap" }; ready[wr]; sleep 5 rescue puts $!.message }
p run(:TERM) { |wr| Thread.new { sleep 0.1; Process.kill(:TERM, $$); puts "kill returned" }; ready[wr]; Queue.new.pop }
p run(:INT)  { |wr| trap(:INT, "IGNORE"); ready[wr]; sleep 0.5; puts "ignored" }
# a signal ignored when the program starts (nohup's HUP) stays ignored
out = IO.popen(["sh", "-c", "trap '' HUP; exec \"$0\" -e 'Process.kill(:HUP, $$); sleep 0.1; puts :alive'", RbConfig.ruby], &:read)
p out
