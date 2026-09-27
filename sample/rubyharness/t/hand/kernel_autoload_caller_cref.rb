module M; def s = autoload(:Q, "/nonexist"); def t = [1].each { autoload(:R, "/n2") }; def k = Kernel.autoload(:K2, "/k2"); end
class C; include M; end
C.new.s; C.new.t; C.new.k
p M.autoload?(:Q), M.autoload?(:R), M.autoload?(:K2), Object.autoload?(:Q), C.autoload?(:Q)
o = Object.new; def o.u = autoload(:Z, "/z"); o.u; p Object.autoload?(:Z)
autoload :TOPX, "/topx"; p Object.autoload?(:TOPX), autoload?(:TOPX)
class D; def q = autoload?(:Q); end; p D.new.q
p Kernel.method(:autoload).owner, Kernel.private_instance_methods.include?(:autoload)
Object.new.instance_eval do
  autoload :KSA, "/ksa"
end
p Object.autoload?(:KSA)
def mm = Object.new.instance_eval { autoload :KSB, "/ksb" }
mm; p Object.autoload?(:KSB)
module MM; def self.x = Object.new.instance_eval { autoload :KSC, "/ksc" }; end; MM.x
p MM.autoload?(:KSC), Object.autoload?(:KSC)
