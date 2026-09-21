# `class X` inside an eval'd string nests in the CREF the eval installed, never
# in the running self -- self can have been rebound by module_exec/class_eval,
# and a binding taken in such a block keeps its own lexical scope.  rspec-mocks'
# receive_spec defines its TestObject with `binding.eval("class TestObject...")`
# from inside a describe block.
module RS
  module MK
  end
end
module Holder
  def self.exec_it(mod, &b) = mod.module_exec(&b)
end
module RS
  module MK
    Holder.exec_it(Class.new) do
      binding.eval("class TestObject; def hi = :hi; end")
      p [:nesting, Module.nesting]
      p [:found, TestObject]
    end
  end
end
p [:mk, RS::MK.const_defined?(:TestObject, false), defined?(::TestObject)]
p RS::MK::TestObject.new.hi

# a binding taken where self was rebound keeps the block's own (top-level) scope
module O; module I; end; end
O::I.module_exec { binding.eval("class T3; end") }
p [O::I.const_defined?(:T3, false), defined?(::T3)]
O::I.module_exec { eval("class T4; end") }
p [O::I.const_defined?(:T4, false), defined?(::T4)]
O::I.class_eval { binding.eval("T5 = 5") }
p [O::I.const_defined?(:T5, false), defined?(::T5)]

# ... but class_eval/instance_eval of a STRING do push the receiver
O::I.class_eval("class B3; end; C3 = 3")
p [O::I.const_defined?(:B3, false), O::I.const_defined?(:C3, false)]
obj = Object.new
obj.instance_eval("class B4; end")
p [obj.singleton_class.const_defined?(:B4, false), defined?(::B4)]

# a real class body still names itself, and `class << obj` names its singleton
module Outer
  module Inner
    def self.go
      binding.eval("class Deep; end")
    end
  end
end
Outer::Inner.go
p [Outer::Inner.const_defined?(:Deep, false), defined?(::Deep)]
# (known gap: a binding taken in a `class << obj` method still nests at the top
# level -- its cref has no name to bake and self is the instance, not the class)

# only Class itself is un-subclassable
begin; eval("class Zq < Class; end"); rescue TypeError => e; p e.message; end
begin; Class.new(Class); rescue TypeError => e; p e.message; end
class Zm < Module; end
p [Zm.new.is_a?(Module), Class.new(Module).new.is_a?(Module)]
