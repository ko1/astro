# `break` directly in a thread's block raises LocalJumpError there (the block can rescue it);
# a loop's break inside it is the loop's; a thread's exception keeps only its own frames (2026-10-16)
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
Thread.report_on_exception = false
t(:first) { th = Thread.new { break 1 }; e = (th.value rescue $!); [e.class, e.message, e.reason, e.exit_value] }
t(:rescued) { Thread.new { begin; break 2; rescue LocalJumpError => e; [:rescued, e.message]; end }.value }
t(:loops) do
  Thread.new do
    a = while true; break 4; end
    b = [1, 2].each { break :each }
    c = for i in 1..3; break i * 10 if i == 2; end
    [a, b, c]
  end.value
end
t(:line) { th = Thread.new {
  x = 1
  break x }; (th.value rescue $!.backtrace.map { File.basename(_1) }) }
def raiser = raise("y")
t(:bt_own_frames) { th = Thread.new { raiser }; [(th.join rescue $!.backtrace.map { File.basename(_1) }), (th.value rescue $!.backtrace_locations.map(&:lineno))] }
t(:proc_break) { pr = proc { break 5 }; (Thread.new { pr.call }.value rescue $!.class) }
