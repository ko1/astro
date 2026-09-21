module A
  module B
    CC = "A::B::CC"
    def self.wrap_mod(target)
      target.module_eval("def f; CC; end", __FILE__, __LINE__)
    end
    # NOTE: instance_eval(String) is deliberately NOT covered here.  CRuby looks
    # in the receiver's singleton class and class hierarchy BEFORE the caller's
    # scope; koruby's flat constant table cannot express that ordering, so it
    # keeps the singleton/hierarchy rule (core/basicobject/instance_eval_spec)
    # and does not fall back to the caller there.
    def self.plain_eval = eval("CC")
  end
end
m = Module.new
A::B.wrap_mod(m)
class K; end
K.include(m)
p (K.new.f rescue [$!.class, $!.message[0,40]])
p A::B.plain_eval
# a NAMED receiver keeps naming itself
module Named; EE = "Named::EE"; end
A::B.module_eval { }
Named.module_eval("def self.h; EE; end", __FILE__, __LINE__)
p Named.h
p (Named.module_eval("CC", __FILE__, __LINE__) rescue [$!.class, $!.message[0,30]])
class Outer
  INNER = 1
  def self.mk = Class.new { }
end
c2 = Outer.mk
p (Outer.class_eval("INNER") rescue $!.class)
