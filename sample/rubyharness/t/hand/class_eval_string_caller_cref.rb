# class_eval / module_eval(String) on a named receiver still sees the CALLER's
# lexical constants after the receiver's own (pry's BasicObject.class_eval inside
# class Object finds Object::ObjectStore).
module M; X = :mx; def self.c = String.class_eval("X"); def self.n = String.class_eval("Module.nesting"); end
p M.c, M.n
module N; Y = :ny; class Inner; Z = :z; end
  def self.d = Inner.class_eval("[Z, Y]")
end
p N.d
X2 = :top; module P; X2 = :p; def self.e = String.class_eval("X2"); end; p P.e
class Object; def self.q = String.class_eval("X2"); end; p Object.q
module R; RR = 1; def self.f = String.class_eval("def __rr = RR"); end; R.f
p "s".__rr
class Object; OS1 = :os; def self.bb = BasicObject.class_eval("binding"); end
b = Object.bb
p b.eval("OS1"), b.eval("self"), b.dup.eval("OS1")
module Q; QQ = 7; def self.bq = String.class_eval("binding"); end
p Q.bq.eval("QQ"), Q.bq.eval("Module.nesting")
