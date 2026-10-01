# super keeps resolving correctly once some file has run `using` (the super
# cache stays on): plain inheritance, include, prepend, a refined method's super.
class A
  def m(x) = "A#{x}"
  def n = "An"
end
module Inc
  def m(x) = "Inc(" + super + ")"
end
class B < A
  include Inc
  def m(x) = "B(" + super + ")"
end
module Pre
  def m(x) = "Pre(" + super + ")"
end
class C < B
  prepend Pre
  def m(x) = "C(" + super + ")"
end

def run = [A.new.m(1), B.new.m(2), C.new.m(3)]
before = run
p before

module R
  refine B do
    def m(x) = "R(" + super + ")"
    def n = "Rn(" + super + ")"
  end
end
p run == before   # not refined here: nothing changes

using R
p B.new.m(4), C.new.m(5), B.new.n
3.times { p [B.new.m(6), C.new.m(7), A.new.m(8)] }

# a method defined after the cache was filled
class B
  def m(x) = "B2(" + super + ")"
end
p run, B.new.m(9)

# a super target defined later
class D < A; def n = "Dn(" + super + ")"; end
p D.new.n
class A; def n = "An2"; end
p D.new.n

# super through extend (a singleton `def o.m` here differs from CRuby already: todo.md)
module E; def m(x) = "E(" + super + ")"; end
q = C.new; q.extend(E)
p q.m(11)
