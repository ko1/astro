module MM
  C1 = :const1
  module Inner; C2 = :const2; end
end
p MM.const_get(:C1)
p MM.const_get("C1")
p MM.const_get("Inner::C2")
p Object.const_get("MM::Inner::C2")
p (MM.const_get("bad-name") rescue $\!.class)
p (MM.const_get("lower") rescue $\!.class)
p (MM.const_get(:Nope) rescue $\!.class)
