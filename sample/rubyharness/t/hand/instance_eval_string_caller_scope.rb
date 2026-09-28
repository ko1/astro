module Cri
  class Err < StandardError; end
  class DSL; end
  def self.a(d) = d.instance_eval("Err")
  def self.b(d, s) = d.instance_eval(s)
  def self.c(d) = DSL.class_eval("Err")
end
p Cri.a(Cri::DSL.new)
p Cri.c(Cri::DSL.new)
p Cri.b(Cri::DSL.new, "Err")

FOO5 = :top
module Inc; QUX = :inc; end
module M5
  FOO = :m
  QUX = :m_qux
  class DSL2; include Inc; end
  def self.run(s) = DSL2.new.instance_eval(s)
end
p M5.run("[FOO, QUX]")

module M6
  FOO = :m
  class Base; FOO = :base; end
  class DSL3 < Base; end
  def self.run(s) = DSL3.new.instance_eval(s)
  def self.run2(s) = DSL3.new.tap(&:singleton_class).instance_eval(s)
end
p M6.run("FOO"), M6.run2("FOO")

class D8; end
def g(s, n) = D8.new.instance_eval(s, n)
def h(*a) = D8.new.instance_eval(a[0], a[1])
def k(*a) = D8.new.instance_eval(*a)
def k2(a) = D8.new.instance_eval(*a)
[:g, :h, :k].each { |m| begin; send(m, 'raise "boom"', 'my.rb'); rescue => e; p e.backtrace.first(3); end }
begin; k2(['raise "boom"', 'my.rb']); rescue => e; p e.backtrace.first(3); end

class H9
  def self.lit(m) = m.instance_eval("X = 1; class P; end; module Q; end; def foo = 1")
  def self.spl(m, *a) = m.instance_eval(*a)
end
m = Module.new; H9.lit(m)
p m.constants.sort, m.singleton_class.constants.sort, m.respond_to?(:foo), H9.constants.sort
m2 = Module.new; H9.spl(m2, "X = 1; class P; end; module Q; end; def foo = 1")
p m2.constants.sort, m2.singleton_class.constants.sort, m2.respond_to?(:foo), H9.constants.sort
o = Object.new; H9.spl(o, "Y = 1; class R; Z = Y; end; R::Z")
p o.singleton_class.constants.sort, H9.constants.sort

m = Module.new
m.instance_eval("X = 1; class P; end; module Q; end; def foo = 1")
p m.constants.sort, m.singleton_class.constants.sort, m.respond_to?(:foo)
o = Object.new
o.instance_eval("Y = 1; class R; end")
p o.singleton_class.constants.sort

module MS
  FOO = :m
  class D; FOO = :d; end
  o = D.new
  class << o; p FOO; def x = FOO; end
  p o.x
end
