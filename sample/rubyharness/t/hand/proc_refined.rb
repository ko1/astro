# Proc#refined (Ruby 4.1): a proc copy whose body runs with refinements active.
module Shout
  refine String do
    def shout = upcase + "!"
  end
end
module Quiet
  refine String do
    def shout = downcase
  end
end

pr = -> s { s.shout }
p pr.refined(Shout).call("hi")
p pr.refined(Shout, Quiet).call("Hi")
p pr.refined(Quiet, Shout).call("Hi")
p pr.refined(Shout).refined(Quiet).call("Hi")
p (pr.refined(Shout) == pr)
p pr.refined.equal?(pr)
begin; pr.call("hi"); rescue NoMethodError => e; p e.class; end

# lambda-ness, arity and the subclass survive the copy
l = pr.refined(Shout)
p [l.lambda?, l.arity]
sub = Class.new(Proc)
p sub.new { |s| s.shout }.refined(Shout).instance_of?(sub)

# refinements reach nested blocks, methods defined inside, and class bodies
p (-> a { a.map { |s| s.shout } }).refined(Shout).call(%w[a b])
p (-> a { a.map(&:shout) }).refined(Shout).call(%w[a b])
p (-> { o = Object.new; def o.hi = "hi".shout; o.hi }).refined(Shout).call
p (-> { Class.new { def hi = "hi".shout }.new.hi }).refined(Shout).call

# instance_eval / instance_exec / class_eval keep them too
q = proc { "hi".shout }.refined(Shout)
p Object.new.instance_eval(&q)
p Object.new.instance_exec(&q)
p Class.new.class_eval(&q)

# dup and clone stay refined
p [l.dup.call("hi"), l.clone.call("hi")]

begin; :upcase.to_proc.refined(Shout); rescue ArgumentError => e; p e.class; end
begin; -> {}.refined(42); rescue TypeError => e; p e.class; end
begin; -> {}.refined(String); rescue TypeError => e; p e.class; end
begin; Class.new { define_method(:m, l) }; rescue ArgumentError => e; p e.class; end
begin; Module.new.module_eval(&proc { using Quiet }.refined(Shout)); rescue RuntimeError => e; p e.class; end
