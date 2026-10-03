# bare `super` in methods with positional parameters only (copied from the frame), and the shapes that keep the Array
class P
  def a = [:a]
  def b(x) = [:b, x]
  def c(x, y = 2) = [:c, x, y]
  def d(x) = [:d, x, block_given? ? yield(x) : :noblk]
  def e(*r) = [:e, r]
  def f(x, k: 1) = [:f, x, k]
  def g(h) = [:g, h]
end
class Q < P
  def a = super + [:q]
  def b(x) = (x = x * 10; super)                # the reassigned local is what super passes
  def c(x, y = 3) = super
  def d(x) = super
  def e(*r) = super
  def f(x, k: 5) = super
  def g(h) = super
end
q = Q.new
p q.a, q.b(1), q.c(1), q.c(1, 9), q.d(4), q.d(4) { _1 + 1 }, q.e(1, 2), q.f(1), q.f(1, k: 7), q.g({ z: 1 })
class R < Q
  def b(x) = [1].map { super }.first             # inside a block: the Array path
  define_method(:c) { |x, y = 4| super(x, y) }
end
p R.new.b(2), R.new.c(1)
class S < P
  def d(x)
    r = super
    GC.start
    r + [x]
  end
end
p 5.times.map { S.new.d(_1) { |v| v * 2 } }.last
p((Class.new(P) { def b(x) = super }.new.b(:ok)))
