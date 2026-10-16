# TracePoint events: line / call / return / b_call / b_return / class / end / raise / rescue / thread_begin /
# thread_end / script_compiled; target:, target_line:, target_thread:, allow_reentry, hook order (2026-10-16)
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
F = __FILE__
def two(x)
  y = x + 1
  y * 2
end
def early(a)
  return a if a
  :no
end
def line_body
  a = 1; b = 2
  two(a)
end
# (only the methods' lines: under AOT the frame that enables keeps running its fused code, no events)
t(:line) do
  l = []
  tp = TracePoint.new(:line) { |q| l << [q.lineno, q.method_id] if q.path == F && q.method_id }
  tp.enable
  begin
    line_body
  ensure
    tp.disable
  end
  l
end
t(:call_return) do
  ev = []
  TracePoint.new(:call, :return) { |q| ev << [q.event, q.lineno, q.method_id, q.defined_class, (q.return_value if q.event == :return), q.parameters] }.enable do
    two(3)
    early(7)
    early(nil)
  end
  ev
end
t(:blocks_classes) do
  ev = []
  TracePoint.new(:b_call, :b_return, :class, :end) { |q| ev << [q.event, q.lineno, (q.parameters if q.event == :b_call)] if q.path == F }.enable do
    [1].each { |v, w = 2| v }
    ->(x, y) { x }.call(1, 2)
    class TpFoo
      Z = 1
    end
  end
  ev
end
t(:raise_rescue) do
  ev = []
  TracePoint.new(:raise, :rescue) { |q| ev << [q.event, q.raised_exception.message] }.enable do
    begin
      raise "boom"
    rescue => e
      e.message
    end
  end
  ev
end
t(:threads) do
  ev = []
  th = nil
  TracePoint.new(:thread_begin, :thread_end) { |q| ev << [q.event, q.self.equal?(th)] if Thread.current == th }.enable(target_thread: nil) do
    th = Thread.new { }
    th.join
  end
  ev
end
t(:script_compiled) do
  ev = []
  TracePoint.new(:script_compiled) { |q| ev << q.eval_script }.enable { eval("1 + 1") }
  ev
end
t(:target) do
  ev = []
  o = Object.new
  def o.foo = bar + [1].map { _1 }.size
  def o.bar = 1
  TracePoint.new(:call, :b_call) { |q| ev << [q.event, q.method_id] }.enable(target: o.method(:foo)) { o.foo }
  ev
end
t(:target_line) do
  ev = []
  pr = -> {
    x = 1
    y = 2
    x + y
  }
  TracePoint.new(:line) { |q| ev << q.lineno }.enable(target: pr, target_line: __LINE__ - 3) { pr.call }
  ev
end
t(:order) do
  ev = []
  a = TracePoint.new(:b_call) { ev << :a }
  b = TracePoint.new(:b_call) { ev << :b }
  target = -> {}
  a.enable(target: target) { b.enable { target.call } }
  ev
end
t(:reentry) do
  ev = []
  TracePoint.new(:line) do |q|
    ev << q.lineno
    next if (__LINE__ + 2..__LINE__ + 3).cover?(q.lineno)
    TracePoint.allow_reentry do
      ra = 1
      rb = 2
    end
  end.enable do
    c = 3
  end
  ev
end
t(:outside) { [(TracePoint.allow_reentry {} rescue $!.class), (TracePoint.new(:line) {}.lineno rescue $!.message)] }
t(:not_supported) { r = nil; TracePoint.new(:line) { |q| r ||= (q.return_value rescue $!.message) }.enable { 1 }; r }
