# String#partition/rpartition coerce a non-String separator via #to_str (and put
# the coerced string in the result). vs ruby.
class TS; def to_str; "-"; end; end
p "a-b-c".partition(TS.new)
p "a-b-c".rpartition(TS.new)
p "a-b-c".partition("-")
p "a-b-c".rpartition("-")
p "hello".partition("x")
p "hello".rpartition("x")
begin; "abc".partition(5); rescue => e; p e.class; end
begin; "abc".rpartition(Object.new); rescue => e; p e.class; end
