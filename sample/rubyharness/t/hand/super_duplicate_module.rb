# super from a module that appears more than once in the ancestors resumes
# after the occurrence it is running as (CRuby: per-iclass)
module W
  def initialize(*a, **k)
    ($log ||= []) << [:W, a, k]
    super
  end
  def v(x) = [:W, defined?(super) ? super(x + 1) : x]
  def boom = super
end
module M
  prepend W
  def self.included(base) = base.prepend(W)
  def v(x) = [:M, x]
end
class Base
  def initialize(*a, **k) = ($log << [:Base, a, k])
  def boom = raise("boom")
end
class C < Base
  include M
end
p C.ancestors.take(5)
$log = []; C.new(1, k: 2); p $log
p C.new.v(1)

class E < Base; include W; prepend W; end
$log = []; E.new(:e); p $log
p E.new.v(10)

module X; def f = [:X, (super rescue :none)]; end
module Y; include X; def f = [:Y, super]; end
class F; include X; include Y; prepend X; end
p F.ancestors.take(4), F.new.f

begin
  C.new.boom
rescue => e
  p e.backtrace_locations.map(&:label).take(3)
end
