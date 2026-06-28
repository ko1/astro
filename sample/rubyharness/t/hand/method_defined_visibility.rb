class C
  def pub; end
  private
  def priv; end
  protected
  def prot; end
end
p C.method_defined?(:pub)
p C.method_defined?(:priv)
p C.method_defined?(:prot)
p C.public_method_defined?(:pub)
p C.private_method_defined?(:priv)
p C.protected_method_defined?(:prot)
p C.private_method_defined?(:pub)
