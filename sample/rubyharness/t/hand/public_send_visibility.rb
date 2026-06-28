class C
  def pub; "pub"; end
  def call_priv; public_send(:priv) rescue "blocked"; end
  protected; def prot; "prot"; end
  private; def priv; "priv"; end
end
o = C.new
p o.public_send(:pub)
p (begin; o.public_send(:priv); rescue NoMethodError; "NME"; end)
p (begin; o.public_send(:prot); rescue NoMethodError; "NME"; end)
p o.send(:priv)
p o.__send__(:prot)
