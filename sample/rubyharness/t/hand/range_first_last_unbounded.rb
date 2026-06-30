# Range#first on beginless / #last on endless raise RangeError. vs ruby.
begin; (..5).first; rescue => e; p [e.class, e.message]; end
begin; (..5).first(2); rescue => e; p [e.class, e.message]; end
begin; (1..).last; rescue => e; p [e.class, e.message]; end
begin; (1..).last(2); rescue => e; p [e.class, e.message]; end
# normal cases still work
p (1..5).first
p (1..5).last
p (1..).first
p (..5).last
p (1..5).first(2)
p (1..5).last(2)
