# A private/protected call that reaches a user #method_missing which calls
# `super` still reports the visibility reason; Module#public_instance_method
# does not go through a singleton #instance_method override.
class MMC
  def method_missing(mid, *a) = super
  private def priv; end
  protected def prot; end
end
p((MMC.new.priv rescue $!.message))
p((MMC.new.prot rescue $!.message))
p((MMC.new.nope rescue $!.message))
p((MMC.new.send(:priv) rescue $!.message))
class MMD; private def priv; end; end
p((MMD.new.priv rescue $!.message))
class MMP; protected def prot; end; def pub; end; end
class MMK
  def self.instance_method(name) = (puts "override"; MMP.instance_method(name))
end
p((MMK.public_instance_method(:prot) rescue $!.message))
p MMP.public_instance_method(:pub).name, (MMP.public_instance_method(:prot) rescue $!.message)
