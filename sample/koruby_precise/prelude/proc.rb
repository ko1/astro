# Proc#curry — partial application.
class Proc
  def curry(n = (arity < 0 ? -arity - 1 : arity))
    acc = nil
    acc = ->(got) { got.length >= n ? call(*got) : ->(*more) { acc.call(got + more) } }
    acc.call([])
  end
  # Function composition: (f >> g).(x) == g(f(x)); (f << g).(x) == f(g(x)).
  # The result is a lambda iff the first-executed proc is (matches CRuby):
  # for >> that is self, for << that is the argument g.
  def >>(g); f = self; lambda? ? ->(*a) { g.call(f.call(*a)) } : proc { |*a| g.call(f.call(*a)) }; end
  def <<(g); f = self; (g.respond_to?(:lambda?) ? g.lambda? : true) ? ->(*a) { f.call(g.call(*a)) } : proc { |*a| f.call(g.call(*a)) }; end
end
class Method
  def >>(g); m = self; ->(*a) { g.call(m.call(*a)) }; end
  def <<(g); m = self; (g.respond_to?(:lambda?) ? g.lambda? : true) ? ->(*a) { m.call(g.call(*a)) } : proc { |*a| m.call(g.call(*a)) }; end
end
