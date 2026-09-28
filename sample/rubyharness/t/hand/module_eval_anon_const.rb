# module_eval(String) on an anonymous module: its constants land in, and resolve from, that module
module G
  module MT
    TPL = <<~RUBY
      def begin_%{m}_event(v)
        Fiber[CURRENT_EV_KEY] = v
      end
      def key_%{m} = CURRENT_EV_KEY
      CURRENT_EV_KEY = :__%{m}_key
      private
      def priv_%{m} = CURRENT_EV_KEY
    RUBY
    def self.create(m) = Module.new.tap { |mod| mod.module_eval(TPL % { m: m }, __FILE__, __LINE__) }
  end
  A = MT.create("aa")
  B = MT.create("bb")
end
class T; include G::B; include G::A; end
t = T.new
p t.key_aa, t.key_bb, t.send(:priv_aa), t.send(:priv_bb), G::A::CURRENT_EV_KEY, G::B::CURRENT_EV_KEY
t.begin_aa_event(1); t.begin_bb_event(2); p Fiber[:__aa_key], Fiber[:__bb_key]
module G2
  module MT
    def self.create
      m = Module.new
      m.module_eval("def mk = Mon.new; def self.cm = Mon", __FILE__, __LINE__)
      m
    end
  end
  Tr = MT.create
  module Tr
    class Mon; end
  end
end
o = Object.new.extend(G2::Tr)
p o.mk.class, G2::Tr.cm
m2 = Module.new; m2.const_set(:K, 1); m2.module_eval("def self.k = K"); p m2.k
c2 = Class.new; c2.class_eval("X = 5; def x = X"); p c2.new.x
