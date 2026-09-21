# A define_method'd method clears the enclosing class_eval/module_exec definee
# for the duration of its body (the body has its own default definee).  The
# saved one has to live somewhere the GC scans: parked in a C local it goes
# stale when the body allocates and the moving GC relocates the class, and the
# NEXT `def` in the enclosing body then lands in freed memory.
#
# rspec hits this on every spec file: `describe`/`it` are define_method'd, so a
# `def helper` written after a nested `describe` in the same group body was lost.
class Runner
  define_method(:run) { |&b| b.call }
  define_method(:run_args) { |n, &b| b.call(n) }
end

def churn(n) = Array.new(n) { |i| { :k => "v#{i}" * 4 } }

Holder = Class.new do
  Runner.new.run { churn(2000) }
  def after_dm = :after_dm
  Runner.new.run_args(3) { |x| churn(2000 * x) }
  def after_dm2 = :after_dm2
end
p Holder.instance_methods(false).sort
p [Holder.new.after_dm, Holder.new.after_dm2]

# the same through module_exec on a named module, with a nested exec in between
module Outer; end
Outer.module_exec do
  Runner.new.run do
    Class.new.module_exec { churn(1000) }
  end
  def nested_ok = :nested_ok
end
p [Outer.instance_methods(false), Outer.private_instance_methods(false)]

# (known gap: a `def` written inside a define_method BODY lands in the global
# function table, not on the block's lexical class -- the body clears the
# definee and self there is an instance, so nothing names the class.)

# class_eval(String) definee survives a define_method'd call too
K2 = Class.new
K2.class_eval <<~RUBY
  Runner.new.run { churn(1500) }
  def from_string = :from_string
RUBY
p [K2.instance_methods(false), K2.new.from_string]
