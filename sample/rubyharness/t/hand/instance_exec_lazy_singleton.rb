# instance_exec / instance_eval blocks make the receiver's singleton class only when something is defined into it
class Foo; def hi; :hi; end; end
a = Foo.new; b = Foo.new
p a.instance_exec(2) { |x| hi.to_s * x }, a.singleton_methods, a.instance_eval { self.class }
a.instance_exec { def solo; :solo; end }
p a.solo, a.singleton_methods, b.respond_to?(:solo)
a.instance_exec { def two; :two; end; alias three two }
p a.three, a.singleton_methods.sort
a.instance_eval { undef three }
p a.respond_to?(:three), a.singleton_methods.sort
b.instance_eval { alias hello hi }
p b.hello, b.singleton_methods
c = Foo.new
c.instance_exec { def d1; :d1; end; c2 = Foo.new; c2.instance_exec { def d2; :d2; end }; p c2.d2, c2.respond_to?(:d1) }
p c.d1, c.respond_to?(:d2)
module Ext; def ext; :ext; end; end
e = Foo.new.extend(Ext)
e.instance_exec { def own; :own; end }
p e.ext, e.own, e.singleton_class.include?(Ext)
begin
  1.instance_exec { def nope; end }
rescue TypeError => err
  p err.class, err.message
end
p 1.instance_exec { self + 1 }, :sym.instance_eval { to_s }
Foo.instance_exec { def cm; :cm; end }
p Foo.cm, Foo.new.respond_to?(:cm)
f = Foo.new.freeze
p f.instance_exec { hi }
begin
  f.instance_exec { def frozen_m; end }
rescue => err
  p err.class
end
g = Foo.new
begin
  g.instance_exec { missing_name }
rescue NameError => err
  p err.message
end
class Base; def run; :base; end; end
class Kid < Base; def run; [super, :kid]; end; end
k = Kid.new
3.times { |i| GC.start if i == 1; p k.instance_exec { run }, k.singleton_methods }
h = Foo.new
pr = proc { def via_proc; :vp; end }
h.instance_exec(&pr)
p h.via_proc, Foo.new.respond_to?(:via_proc)
