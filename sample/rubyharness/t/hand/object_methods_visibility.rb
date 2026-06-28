module Mix; def mixed; end; end
class Base; def binh; end; private; def basepriv; end; end
class C < Base
  include Mix
  def pub1; end
  def pub2; end
  protected; def prot1; end
  private; def priv1; end
end
o = C.new
def o.singdef; end
p o.public_methods(false).sort
p o.private_methods(false).sort
p o.protected_methods(false).sort
p (o.methods.include?(:pub1) && o.methods.include?(:mixed) && o.methods.include?(:binh))
p o.methods.include?(:priv1)
p o.methods.include?(:singdef)
p o.public_methods.include?(:mixed)
