# Refinements are lexical: a Proc carries the set active where its body was
# WRITTEN, so calling it from an unrefined scope still sees the refinement.
module Shout
  refine String do
    def shout = upcase + "!"
  end
  refine Hash do
    def to_s = "<" + keys.join(",") + ">"
  end
end

module Caller                       # no `using` here
  def self.run(pr, *a) = pr.call(*a)
  def self.exec(pr) = Object.new.instance_exec(&pr)
  def self.mod_exec(pr) = Module.new.module_exec(&pr)
end

PLAIN = proc { "hi".shout }         # before the `using`, but shares the toplevel cref

using Shout

p Caller.run(proc { |s| s.shout }, "hi")
p Caller.run(lambda { |s| s.shout }, "hi")
p Caller.run(proc { |h| "#{h}" }, {a: 1, b: 2})     # interpolation sees the refined #to_s
p Caller.exec(proc { "hi".shout })
p Caller.mod_exec(proc { "hi".shout })

# nested blocks and a block that is only reified on the way out
outer = proc { |a| a.map { |s| s.shout } }
p Caller.run(outer, %w[a b])
def keep(&b) = b
p Caller.run(keep { "x".shout })

# define_method with a block written here is fine (it is not a Proc#refined copy)
c = Class.new { define_method(:go) { "hi".shout } }
p Caller.run(proc { c.new.go })

p %w[a].map(&:shout)

# a toplevel `using` reaches back: the cref it mutates is the one PLAIN captured
p PLAIN.call
