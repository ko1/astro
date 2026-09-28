# io/console: VT sequences, console_mode (IO::Console::Mode), ttyname, input_pending?, the Windows-only
# three raise NotImplementedError; StringIO#getch / getpass come with it; IO has no truncate / codepoints
require "io/console"; require "pty"
r, w = IO.pipe
%i[beep clear_screen hide_cursor show_cursor].each { |m| w.send(m) }
w.erase_line(1); w.erase_screen(2); w.goto(3, 4); w.goto_column(5); w.cursor_up(2); w.cursor_down(1); w.cursor_left(3); w.cursor_right(4); w.scroll_forward(2); w.scroll_backward(1); w.cursor = [1, 2]; w.close
p r.read
r2, w2 = IO.pipe; w2.write "x"; p r2.input_pending?, r2.ttyname
%i[console_input_events check_winsize_changed].each { |m| p((begin; r2.send(m); rescue Exception => e; [e.class, e.message]; end)) }
PTY.open do |m, s|
  p s.input_pending?; m.write "a"; sleep 0.05; p s.input_pending?
  md = s.console_mode; p md.echo?, md.class, md.min, md.time
  md.echo = false; s.console_mode = md; p s.echo?
  p s.ttyname.start_with?("/dev/pts/")
  rm = md.raw(min: 2); p rm.min, md.min, rm.echo?
end
p IO::Console::Mode.equal?(IO::ConsoleMode)
require "io/console"; require "stringio"
s = StringIO.new(+"ab\ncd\n"); p s.getpass, s.getpass, s.string
t = StringIO.new(+"xy\nzz\n"); p t.getpass("P:"), t.string, t.getch
p IO.method_defined?(:truncate), IO.method_defined?(:codepoints), File.method_defined?(:truncate)
require "stringio"; p StringIO.method_defined?(:chars), StringIO.method_defined?(:bytes)
