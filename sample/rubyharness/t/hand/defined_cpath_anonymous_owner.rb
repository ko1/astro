# defined?(A::B) with A only a constant of an anonymous module: no bare name reaches A.
m = Module.new; m.const_set(:Foo, 1); Module.new.const_set(:Nope, m)
p defined?(Nope), defined?(Nope::Foo)
module Real; X = 1; end
p defined?(Real::X), defined?(Real::Y), defined?(::Real::X)
a = Module.new; a.const_set(:In, Module.new); a::In.const_set(:Z, 2)
p defined?(a::In::Z), defined?(In::Z)
m = Module.new; m.const_set(:Foo, 1); Module.new.const_set(:Nope, m); begin; p Nope::Foo; rescue NameError => e; p e.class; end; class Outer; module Inner; Q=1; end; def self.t = defined?(Inner::Q); end; p Outer.t
