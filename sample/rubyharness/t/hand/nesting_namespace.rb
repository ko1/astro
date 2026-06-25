# Module.nesting + namespaced class/module names (rubyspec follow-up)
module NN_Foo
  module NN_Bar
    NESTING_A = Module.nesting
  end
end
p NN_Foo::NN_Bar::NESTING_A.length
p NN_Foo::NN_Bar::NESTING_A.all? { |m| m.is_a?(Module) }
p Module.nesting

class NN_Baz
  N_IN_CLASS = Module.nesting
  def m; Module.nesting; end
end
p NN_Baz::N_IN_CLASS == [NN_Baz]
p NN_Baz.new.m == [NN_Baz]

# namespaced class / module names (flat const table)
module Outer1; end
module Outer1::Inner1
  def self.hi; "inner"; end
end
p Outer1::Inner1.hi
class Outer1::Klass1
  def km; "km"; end
end
p Outer1::Klass1.new.km
