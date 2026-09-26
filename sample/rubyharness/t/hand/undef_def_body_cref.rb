# `undef` / `def` inside a method body act on its cref: the owner, but a
# `def obj.m` / `def self.m` body (a non-class singleton) uses the lexical one —
# Object at the top level (pry evaluates in a toplevel binding taken in a method).
class Object; def ls2 = 6; end
def self.m2; undef ls2; end
m2; p Object.method_defined?(:ls2)
def tl_u = 1
undef tl_u
p((tl_u rescue $!.class))
class K; def a = 1; def b = 2; def kill; undef b; end; end
K.new.kill; p K.method_defined?(:b), K.new.a
o = Object.new; r = begin; o.instance_eval { undef inspect }; :ok; rescue => e; [e.class, e.message]; end; p r
r = begin; 1.instance_eval("undef to_s"); :ok; rescue => e; [e.class, e.message]; end; p r
def self.m3; def inner3 = 3; end
m3; p Object.public_method_defined?(:inner3), singleton_class.method_defined?(:inner3, false)
class C; def self.mk; def in_c = 1; end; end
C.mk; p C.public_method_defined?(:in_c), C.singleton_class.method_defined?(:in_c, false)
o = Object.new; def o.mk = (def in_o = 1); o.mk; p Object.public_method_defined?(:in_o)
class DN; OBJ = Object.new; def OBJ.mk = (def in_dn = 1); end
DN::OBJ.mk; p DN.public_method_defined?(:in_dn)
o2 = Object.new; o2.instance_eval { def mk2 = (def in_ie = 1) }; o2.mk2
p o2.singleton_class.method_defined?(:in_ie, false), Object.method_defined?(:in_ie)
