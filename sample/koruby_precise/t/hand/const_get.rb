BAUD = 9600
class Widget; def hi; "w"; end; end
DB = { v: :Widget }
p Object.const_get(:Widget).new.hi
p Object.const_get("BAUD")
p Object.const_defined?(:Widget)
p Object.const_defined?(:Nope)
k = DB[:v]
p Object.const_get(k).new.hi
begin; Object.const_get(:Missing); rescue => e; puts e.class; end
