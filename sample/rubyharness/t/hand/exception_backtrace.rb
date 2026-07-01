# Exception#backtrace returns an Array<String> once raised (nil before), and
# set_backtrace round-trips. Line-exact content is checked against ruby for the
# common explicit-raise shape. vs ruby.

# not-yet-raised exception has a nil backtrace
p RuntimeError.new("x").backtrace

# rescued exception exposes an Array of Strings
bt = nil
begin
  raise "boom"
rescue => e
  bt = e.backtrace
end
p bt.is_a?(Array)
p bt.all? { |s| s.is_a?(String) }
p bt.last.include?("<main>")

# backtrace is captured once and preserved across re-raise
outer = nil
begin
  begin
    raise "inner"
  rescue => e1
    outer = e1.backtrace
    raise
  end
rescue => e2
  p e2.backtrace == outer
end

# set_backtrace accepts an array / string / nil
err = RuntimeError.new("y")
err.set_backtrace(["a.rb:1:in 'foo'"])
p err.backtrace
err.set_backtrace(nil)
p err.backtrace

# Kernel#raise is never itself present in the trace
def deep; raise "z"; end
begin
  deep
rescue => e
  p e.backtrace.first.include?("raise")
end
