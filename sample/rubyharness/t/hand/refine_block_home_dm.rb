# a block written under `using` keeps those refinements when another (unrefined)
# method reifies it and define_method's it — also after the writing method returned
class Object; def self.itx(&b) = define_method(:t, &b); def self.keep(&b) = (@kept = b); def self.kept = @kept; end
module TM
  refine Array do
    def name = self[0]
  end
end
using TM
module LE
  def self.included(child) = child.module_eval { itx { [7, 8].name } }
  def self.direct(k) = k.itx { [9].name }
  def self.later(k) = k.keep { [5].name }
end
k = Class.new { include LE }; p k.new.t
k2 = Class.new; LE.direct(k2); p k2.new.t
k3 = Class.new; LE.later(k3); GC.start; p k3.kept.call
