# eval'd `def` inside a method goes to that method's cref, even under instance_exec
Object.new.instance_exec do
  class EvDefA
    def m
      eval "def am; self; end", binding
      am
    end
  end
end
o = EvDefA.new
p o.m.equal?(o), EvDefA.instance_methods(false).sort

# a method of `class << self`: the def is a singleton method, constants still resolve
module EvOuter
  class EvDefB
    X = 1
    class << self
      def cm = (eval "def acm; self; end"; acm)
      def cb = binding
      def cx = binding.eval("X")
    end
  end
end
p EvOuter::EvDefB.cm, EvOuter::EvDefB.singleton_methods.sort, EvOuter::EvDefB.instance_methods(false)
EvOuter::EvDefB.cb.eval("def acm2 = 2"); p EvOuter::EvDefB.acm2
p EvOuter::EvDefB.cx

# a block inside module_eval keeps the module as the definee (ERB's def_method)
module EvMod; end
def erb_like(mod) = mod.module_eval { eval("def from_erb = 3", binding) }
erb_like(EvMod); p EvMod.instance_methods(false)
