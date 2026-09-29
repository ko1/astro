# implicit-self send / __send__ / public_send honour a user definition (class, singleton, include, extend) — answered from per-class bits
class A; def go = send(:x); def x = :a_x; end
class B < A; def send(*a) = :b_send; end
p A.new.go, B.new.go
o = A.new; def o.send(*) = :sing; p o.go
module M; def send(*) = :m_send; end
a2 = A.new; p a2.go; A.include(M); p a2.go, A.new.go
class C; def go = __send__(:y); def y = :cy; def public_send(*) = :cps; def go2 = public_send(:y); end
p C.new.go, C.new.go2
e = Object.new; e.extend(Module.new { def send(*) = :ext }); def e.go = send(:z); p e.go
p send(:format, "%d", 3)
