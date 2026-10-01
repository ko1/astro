# A top-level def called bare from inside an object's method (cached after the first call):
# self is the object, a later same-named method in the class wins, undef_method hides it,
# a BasicObject subclass goes to method_missing, and one site sees several classes.
# Run under both and diff.
def tl(x) = [:tl, x, self.class]
def tl2(x) = x < 1 ? tl(x) : tl2(x - 1)
class A
  def run = [tl(1), tl(2), tl2(3)]
  def run2 = tl(:a)
end
a = A.new
p a.run, a.run
class A
  def tl(x) = [:own, x]
end
p a.run
class B2
  def go = 3.times.map { tl(it) }
end
p B2.new.go
class C2
  undef_method :tl rescue p $!.class
  def go = (tl(1) rescue $!.class)
end
p C2.new.go
class BO < BasicObject
  def method_missing(n, *a) = [:mm, n, a]
  def go = tl(5)
end
p BO.new.go
module Shared
  def callit = tl(:shared)
end
class D; include Shared; end
class E; include Shared; end
p [D.new.callit, E.new.callit, D.new.callit]
def tl(x) = [:redef, x]
p B2.new.go.first, D.new.callit
