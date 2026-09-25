# A raise inside a string eval keeps the whole backtrace: the eval frame (labelled
# with the method it runs in), Kernel#eval / instance_eval, and every caller.
def a; eval("raise 'foo'"); end
def b; a; end
begin; b; rescue => e; puts e.backtrace; end
puts "--"
def c; eval("1\n2\nraise 'bar'", binding, "tmpl.erb", 10); end
begin; c; rescue => e; puts e.backtrace; end
puts "--"
def d; Object.new.instance_eval("raise 'baz'"); end
begin; d; rescue => e; puts e.backtrace; end
puts "--"
require "erb"
def f; ERB.new("x\n<%= raise 'erb' %>").result(binding); end
begin; f; rescue => e; puts e.backtrace.first; p e.backtrace.any? { |l| l.include?("Kernel#eval") }; end
