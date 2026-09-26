# exit! drops output still in Ruby's buffer (CRuby); p flushes, sync'd IO and
# stderr are unbuffered, IO.popen("-")'s child has a synchronized stdout, and
# IO#reopen(io) takes the donor's sync (a pipe's write end is sync).
%w[p(1) puts(1) $stdout.write("w") pp(2)].each do |code|
  io = IO.popen("-")
  if io
    p [code, io.read]; io.close
  else
    eval(code); exit!(0)
  end
end
r, w = IO.pipe
pid = fork { r.close; $stdout.reopen(w); puts "lost"; p :kept; exit!(0) }
w.close; Process.wait(pid); p r.read
r, w = IO.pipe
pid = fork { r.close; $stdout.reopen(w); puts "gone"; exit!(0) }
w.close; Process.wait(pid); p r.read
