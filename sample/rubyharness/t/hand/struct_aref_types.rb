# Struct#[] index types: Integer/Float index, Symbol/String member, else TypeError. vs ruby.
S = Struct.new(:make, :model)
c = S.new("Ford", "Ranger")
p c[0]; p c[1]; p c[-1]; p c[1.5]
p c[:make]; p c["model"]
begin; c[Object.new]; rescue => e; p e.class; end
begin; c[5]; rescue => e; p e.class; end
begin; c[2.5]; rescue => e; p e.class; end
begin; c[:bad]; rescue => e; p [e.class, e.message]; end
begin; c["nope"]; rescue => e; p [e.class, e.message]; end
