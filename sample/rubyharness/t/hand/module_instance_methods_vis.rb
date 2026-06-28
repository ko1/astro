class C
  def pub; end
  protected; def prot; end
  private; def priv; end
end
p C.public_instance_methods(false).sort
p C.private_instance_methods(false).sort
p C.protected_instance_methods(false).sort
p C.instance_methods(false).sort
