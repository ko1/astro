# trace_var hooks, a signal's default exception deferred by Thread.handle_interrupt, other threads
# killed at exit (their ensure runs), require of a broken CRuby .so, symlinked require_relative /
# $LOAD_PATH (2026-10-16)
require "tmpdir"
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
t(:trace_var) do
  log = []
  trace_var(:$tv_x) { |v| log << [:a, v] }
  trace_var(:$tv_x, proc { |v| log << [:b, v] })
  $tv_x = 1
  trace_var(:$tv_x, "$tv_y = :from_string")
  $tv_x = 2
  removed = untrace_var(:$tv_x).size
  $tv_x = 3
  [log, $tv_y, removed]
end
t(:trace_var_noblock) { trace_var(:$tv_x) }
t(:trace_var_private) { [Kernel.private_instance_methods(false).include?(:trace_var), Kernel.public_methods(false).include?(:trace_var)] }
t(:signal_deferred) do
  waiting = Thread::Queue.new; release = Thread::Queue.new; inner = false
  Thread.new { waiting.pop; Process.kill(:INT, Process.pid); release.push(true) }
  begin
    Thread.handle_interrupt(SignalException => :never) do
      begin
        waiting.push(true); release.pop
      rescue SignalException
        inner = true; raise
      end
    end
  rescue SignalException => e
    [e.class, e.signo, inner]
  end
end
t(:cv_wakes_after_trap) do
  old = trap(:USR1) { }
  q = Thread::Queue.new
  Thread.new { Process.kill(:USR1, Process.pid); q.push(:got) }
  r = q.pop
  trap(:USR1, old)
  r
end
t(:broken_so) do
  Dir.mktmpdir do |d|
    File.write(File.join(d, "brk.so"), "")
    begin; require File.join(d, "brk"); rescue LoadError => e; [e.message.sub(d, "D").sub(d, "D"), e.path]; end
  end
end
t(:symlink_require_relative) do
  Dir.mktmpdir do |d|
    Dir.mkdir(File.join(d, "real"))
    File.write(File.join(d, "real", "a.rb"), "require_relative 'b'")
    File.write(File.join(d, "real", "b.rb"), "$sym_b = :loaded")
    File.symlink(File.join(d, "real", "a.rb"), File.join(d, "link.rb"))
    require_relative File.join(d, "link.rb")
    $sym_b
  end
end
t(:symlink_load_path) do
  Dir.mktmpdir do |d|
    real = File.realpath(d)
    Dir.mkdir(File.join(real, "realdir"))
    File.write(File.join(real, "realdir", "realfile.rb"), "$sym_file = __FILE__")
    File.symlink("realfile.rb", File.join(real, "realdir", "symfile_1016.rb"))
    File.symlink(File.join(real, "realdir"), File.join(real, "symdir"))
    $LOAD_PATH.unshift(File.join(real, "symdir"))
    begin
      [require("symfile_1016"), $sym_file == File.join(real, "realdir", "symfile_1016.rb"),
       $".last == File.join(real, "realdir", "symfile_1016.rb"), $LOAD_PATH[0] == File.join(real, "symdir"),
       require("symfile_1016")]
    ensure
      $LOAD_PATH.shift
    end
  end
end
Thread.new do
  begin
    Fiber.new { begin; Fiber.yield; ensure; puts "suspended fiber ensure"; end }.resume
    $exit_ready = true
    sleep
  ensure
    puts "thread ensure at exit"
  end
end
Thread.pass until $exit_ready
