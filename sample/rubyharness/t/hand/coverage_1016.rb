# Coverage: line / method / oneshot coverage of files loaded and Strings evaluated after start (2026-10-16).
# CRuby's own coverage.so is taken directly: the reference install's coverage.rb is a harness stub.
begin
  require "coverage.so"
rescue LoadError
  require "coverage"
end
require "tmpdir"
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
DIR = Dir.mktmpdir
def src(name, code) = File.join(DIR, name).tap { File.write(_1, code) }
A = src("cov_a.rb", "#comment\nclass CovA\n\n  def m\n    x = 1\n\n  end\n\n  def unused\n    2\n  end\nend\nCovA.new.m\nbegin\n  y = 3\nend\n[1, 2].each do |q|\n  q\nend\n")
B = src("cov_b.rb", "module CovB\n  class_eval <<-RUBY, __FILE__, __LINE__ + 1\n    def z = 1\n  RUBY\nend\n")
def rel(h) = h.transform_keys { File.basename(_1) }
t(:lines) { Coverage.start; load A; rel(Coverage.result) }
t(:all) { Coverage.start(:all); load A; r = rel(Coverage.result); r.transform_values { |v| v.merge(methods: v[:methods].map { |k, c| [k.drop(1), c] }.sort) } }
t(:eval_on) { Coverage.start(lines: true, eval: true); load B; rel(Coverage.result) }
t(:eval_off) { Coverage.start(lines: true, eval: false); load B; rel(Coverage.result) }
t(:eval_named) { Coverage.start(lines: true, eval: true); eval("1\n2\n", binding, "cov_named.rb", 1); Coverage.result["cov_named.rb"] }
t(:oneshot) { Coverage.start(oneshot_lines: true); load A; rel(Coverage.result) }
t(:peek_clear) do
  Coverage.start; load A
  a = rel(Coverage.peek_result)
  rel(Coverage.result(stop: false, clear: true))
  b = rel(Coverage.peek_result)
  Coverage.result
  [a, b]
end
t(:not_enabled) { Coverage.result }
t(:twice) { Coverage.start; begin; Coverage.start; rescue => e; e.message; ensure; Coverage.result; end }
t(:both_lines) { Coverage.start(lines: true, oneshot_lines: true) }
t(:not_hash) { Coverage.start(42) }
t(:suspend_resume) do
  Coverage.setup; r = [Coverage.running?]
  load A
  Coverage.resume; r << Coverage.running?; CovA.new.m
  Coverage.suspend; r << Coverage.running?; CovA.new.m
  r << rel(Coverage.result)
end
t(:supported) { [Coverage.supported?(:lines), Coverage.supported?(:foo), (Coverage.supported?("lines") rescue $!.class)] }
