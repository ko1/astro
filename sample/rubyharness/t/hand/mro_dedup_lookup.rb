# `include` keeps the DEEPEST copy of a module already in the chain, so method
# lookup must follow the deduped ancestry, not a per-include depth-first walk.
# rake hits this: RakefileDefinitions re-includes FileUtils above Rake::DSL.
module FU;  def m; "FU";  end; def only_fu; "only"; end; end
module FUE; include FU; def m; "FUE"; end; end
module DSL; include FUE; end
module RD;  include FU;  end

class C
  include DSL
  include RD
end
p C.ancestors - Object.ancestors
p C.instance_method(:m).owner
p C.new.m
p C.new.only_fu

# the deeper copy also wins for super
module Base; def who; "Base"; end; end
module Mid;  include Base; def who; "Mid-" + super; end; end
module Top;  include Base; end
class D
  include Mid
  include Top
end
p D.new.who

# a fresh chain with no repeat is unaffected
module P1; def z; "P1"; end; end
module P2; include P1; def z; "P2-" + super; end; end
class E; include P2; end
p E.new.z
p E.instance_method(:z).owner

# singleton chains (extend) follow the same rule
o = Object.new
o.extend DSL
o.extend RD
p o.m
p o.singleton_class.ancestors.include?(FUE)
