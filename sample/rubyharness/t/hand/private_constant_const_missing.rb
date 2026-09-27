mod = Module.new
mod.const_set :Foo, true
mod.send :private_constant, :Foo
def mod.const_missing(name) = name == :Foo ? name : super
p mod::Foo
m2 = Module.new; m2.const_set :Bar, 1; m2.send :private_constant, :Bar
p((m2::Bar rescue [$!.class, $!.message.sub(/#<Module:0x\h+>/, "M")]))
class K; X = 1; private_constant :X; def self.const_missing(n) = [:cm, n]; end
p K::X
class K2 < K; end; p K2::X
