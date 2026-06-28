class C
  def pub; end
  private
  def priv; end
  protected
  def prot; end
  public
  def pub2; end
  def named_priv; end
  private :named_priv
end
o = C.new
p o.respond_to?(:pub)
p o.respond_to?(:pub2)
p o.respond_to?(:priv)
p o.respond_to?(:priv, true)
p o.respond_to?(:prot)
p o.respond_to?(:prot, true)
p o.respond_to?(:named_priv)
