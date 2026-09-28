require 'pty'
require 'io/wait'

# PTY.open gives a real terminal pair; a write to a tty IO goes out at once
PTY.open do |m, s|
  p [m.class, s.class, s.tty?, m.tty?, s.path.start_with?("/dev/")]
  o = IO.new(s.fileno, "w", autoclose: false)
  o.write "abc"                       # no flush, no sync
  p o.sync
  p m.wait_readable(2) ? m.read_nonblock(10) : :nothing
  s.puts "line"
  p m.gets
end

# PTY.spawn: the child's stdio is the slave, so it sees a tty
PTY.spawn("test -t 0 && test -t 1 && echo tty; exit 3") do |r, w, pid|
  p r.gets
  Process.wait(pid)
  p $?.exitstatus
  p((r.read rescue $!.class))
end

require 'objspace'
p ObjectSpace.allocation_sourcefile(Object.new)
