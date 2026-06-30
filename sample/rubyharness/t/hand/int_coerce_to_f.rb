# Integer#coerce(obj): Integer -> [obj, self]; else Float(obj) via #to_f, with a
# TypeError if #to_f returns a non-Float. vs ruby.
class F; def to_f; 8.0; end; end
p 5.coerce(F.new)
p 5.coerce(2.5)
p 5.coerce(3)
p 5.coerce("2.5")
p (10**40).coerce(F.new)
class B; def to_f; "0"; end; end
begin; 5.coerce(B.new); rescue => e; p e.class; end
begin; 5.coerce("x"); rescue => e; p e.class; end
begin; 5.coerce(Object.new); rescue => e; p e.class; end
