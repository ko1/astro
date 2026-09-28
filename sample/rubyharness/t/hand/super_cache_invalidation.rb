class A; def m = [:A]; end
class B < A; def m = [:B] + super; end
b = B.new
p b.m
class A; def m = [:A2]; end
p b.m
module M; def m = [:M] + super; end
class B; include M; end
p b.m
module P; def m = [:P] + super; end
class B; prepend P; end
p b.m
c = B.new; def c.m = [:sing] + super; p c.m
class C < B; alias_method :m2, :m; def m = [:C] + super; end
p C.new.m, C.new.m2
module N; def m = [:N] + super; end
b.extend(N); p b.m
p B.new.m
class A; remove_method :m; end
p((B.new.m rescue $!.class))
class A; def m = [:A3]; end
p B.new.m
GC.start
p B.new.m, C.new.m
