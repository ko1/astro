# A raise that LEAVES a require'd/load'ed file: the file's own '<top (required)>'
# frame must stay in the backtrace, and the require/load C frame between two
# files must be reported in the REQUIRER's file (it has no source of its own).
# The rescue is deliberately outside every block, because a `block in ...` frame
# is a separate, still-open gap on the unwind path.  Run under CRuby and koruby
# and diff.
D = __dir__

def report(tag, e)
  puts "== #{tag}"
  e.backtrace.each { |l| puts "  #{l.sub(D + '/', '')}" }
  (e.backtrace_locations || []).each { |l| puts "  loc #{l.path.sub(D + '/', '')}:#{l.lineno}:in '#{l.label}'" }
end

begin
  load File.expand_path('t2_unwind_raise.rb', D)
rescue => e
  report('load', e)
end

begin
  load File.expand_path('t2_unwind_raise.rb', D), true
rescue => e
  report('load wrap', e)
end

begin
  require File.expand_path('t2_unwind_raise.rb', D)
rescue => e
  report('require', e)
end

begin
  require_relative 't2_unwind_raise'
rescue => e
  report('require_relative', e)
end

begin
  load File.expand_path('t2_unwind_top.rb', D)
rescue => e
  report('raise at file top level', e)
end

# two file boundaries at once: the middle file's frame and its require both have
# to land, and the inner require is reported in the MIDDLE file.
begin
  require_relative 't2_unwind_mid'
rescue => e
  report('nested require', e)
end

# the require sits inside a method, so the chain continues into it
def t2_via_method
  require_relative 't2_unwind_raise'
end
begin
  t2_via_method
rescue => e
  report('require in a method', e)
end
