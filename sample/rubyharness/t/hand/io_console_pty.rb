# io/console works on a PTY slave through its own fd (not the controlling tty),
# and raises ENOTTY on a pipe
require "io/console"
require "pty"
r, w = IO.pipe
%i[winsize echo? getch].each { |m| begin; r.send(m); rescue => e; p [m, e.class]; end }
begin; r.raw {}; rescue => e; p [:raw, e.class]; end
PTY.open do |m, s|
  s.winsize = [30, 100]
  p s.winsize, s.echo?
  s.noecho { p s.echo? }
  s.raw { p s.echo? }
  p s.echo?
  s.echo = false; p s.echo?; s.echo = true
  s.raw! ; p s.echo?; s.cooked!; p s.echo?
  m.write "ab"; p s.getch, s.getch
end
