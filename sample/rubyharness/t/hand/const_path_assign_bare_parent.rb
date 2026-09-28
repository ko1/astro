class SubMod < ::Module
  def initialize(tag)
    super()
    inner = ::Module.new
    inner.const_set(:Tag, tag)
    const_set(:Coercible, inner)
  end
end
A = SubMod.new(:a)
B = SubMod.new(:b)
module T3
  include B
  p Coercible::Tag
  Coercible::JH = 1
end
p B::Coercible.const_defined?(:JH), A::Coercible.const_defined?(:JH)
X = 1
p((begin; X::Y = 2; rescue => e; [e.class, e.message]; end))
module M; end
M::Z = 3
p M::Z, M.const_source_location(:Z)
module Foo2; end
Foo2::X = 1
Foo2::X = 2 rescue nil
p Foo2::X, Foo2.const_source_location(:X)&.last
module Foo2; Foo2::Y = Class.new; end
p Foo2::Y.name
class Bar; Baz = Module.new; Baz::Q = 3; end
p Bar::Baz::Q, Bar::Baz.name
