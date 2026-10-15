# spawn of a #to_str command line, closed STDERR raises, trap PIPE values, Process.setpgrp,
# exec's fd => fd clearing close-on-exec (2026-10-16)
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
t(:spawn_to_str) { o = Object.new; def o.to_str = "echo foo"; r, w = IO.pipe; pid = Process.spawn(o, out: w); w.close; Process.wait(pid); r.read }
t(:trap_pipe) { a = Signal.trap("PIPE", "IGNORE"); b = Signal.trap("PIPE", "DEFAULT"); c = Signal.trap("PIPE", "DEFAULT"); [a, b, c] }
t(:setpgrp) { Process.respond_to?(:setpgrp) }
t(:exec_cloexec) {
  f = File.open(__FILE__)
  before = f.close_on_exec?
  Process.exec("/", f.fileno => f.fileno) rescue nil
  [before, f.close_on_exec?]
}
