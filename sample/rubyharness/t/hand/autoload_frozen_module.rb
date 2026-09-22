o = Object.new
sc = o.singleton_class
sc.freeze
p (sc.autoload?(:Nope) rescue [$!.class, $!.message[0,40]])
p (sc.autoload?(:Nope, false) rescue [$!.class, $!.message[0,40]])
m = Module.new
m.autoload(:Zz, "/tmp/kz/does_not_exist.rb")
p m.autoload?(:Zz)
m.freeze
p m.autoload?(:Zz)
p (Object.autoload?(:Nothing) rescue $!.class)
class Fz; end
Fz.freeze
p (Fz.autoload?(:Q) rescue [$!.class, $!.message[0,30]])
module AA; end
AA.autoload(:BB, "/tmp/kz/nope.rb")
p AA.autoload?(:BB)
class Sub2 < Fz; end rescue p $!.class
