# String#clamp accepts the 1-arg Range form (like numeric clamp), and a single
# non-Range arg raises TypeError (expected Range). vs ruby.
p "m".clamp("a".."z")
p "z".clamp("a".."m")
p "a".clamp("m".."z")
p "m".clamp("a", "z")
p "m".clamp(.."d")
p "m".clamp("p"..)
begin; "m".clamp("x"); rescue => e; p [e.class, e.message]; end
begin; "m".clamp("a", "b", "c"); rescue => e; p e.class; end
