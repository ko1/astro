class C
  def pm(*a) = "pm#{a.inspect}"
  private :pm
  def prot = "prot"
  protected :prot
end
c = C.new
b = proc { 1 }
arr = [1,2]
p (c.pm rescue $!.class)
p (c.pm { } rescue $!.class)
p (c.pm(&b) rescue $!.class)
p (c.pm(*arr) rescue $!.class)
p (c.pm(*arr) { } rescue $!.class)
p (c&.pm rescue $!.class)
p (c&.pm { } rescue $!.class)
p (c.prot rescue $!.class)
p (c.prot { } rescue $!.class)
p (c.public_send(:pm) rescue $!.class)
module Km
  def kpriv = "kpriv"
  private :kpriv
end
class String; include Km; end
p ("s".kpriv { } rescue [$!.class, $!.message])
p ("s".kpriv(&proc{}) rescue $!.class)
p (send(:p) rescue $!.class)
class D
  def go(o) = o.send(:pm)          # send bypasses privacy
  def go2(o) = o.pm { }
end
class E < C
  def peer(o) = o.prot { }          # protected: allowed between kin
end
p D.new.go(C.new)
p (D.new.go2(C.new) rescue $!.class)
p E.new.peer(E.new)
p (E.new.peer(C.new) rescue $!.class)
