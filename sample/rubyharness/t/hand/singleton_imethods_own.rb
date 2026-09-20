# Module#instance_methods(false) and friends report only the RECEIVER's own
# table — a singleton class is not an exception (koruby used to keep walking the
# superclass chain for singletons, so K.singleton_class.instance_methods(false)
# returned all of Class's).
class K
  def self.a; end
  def self.b; end
end
p K.singleton_class.instance_methods(false).sort
p K.singleton_class.private_instance_methods(false).sort
p K.singleton_class.public_instance_methods(false).sort
p K.singleton_class.protected_instance_methods(false).sort

o = Object.new
def o.x; end
p o.singleton_class.instance_methods(false).sort
p o.singleton_methods.sort
p o.methods(false).sort

m = Module.new { def self.z; end }
p m.singleton_class.instance_methods(false).sort

module Mx; def x; end; private def y; end; end
class C
  include Mx
  def a; end
  private def b; end
  protected def c; end
end
p C.instance_methods(false).sort
p C.private_instance_methods(false).sort
p C.protected_instance_methods(false).sort
p C.public_instance_methods(false).sort
p C.instance_methods.include?(:x)
p Comparable.instance_methods(false).sort
p Integer.instance_methods(false).size > 20

D = Data.define(:a, :b)
class << D
  private :[]
end
p D.singleton_class.private_instance_methods(false).sort
