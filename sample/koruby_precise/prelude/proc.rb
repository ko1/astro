# Proc#curry — partial application.
class Proc
  def curry(n = (arity < 0 ? -arity - 1 : arity))
    acc = nil
    acc = ->(got) { got.length >= n ? call(*got) : ->(*more) { acc.call(got + more) } }
    acc.call([])
  end
  # Function composition: (f >> g).(x) == g(f(x)); (f << g).(x) == f(g(x)).
  def >>(g); f = self; ->(*a) { g.call(f.call(*a)) }; end
  def <<(g); f = self; ->(*a) { f.call(g.call(*a)) }; end
end
class Method
  def >>(g); m = self; ->(*a) { g.call(m.call(*a)) }; end
  def <<(g); m = self; ->(*a) { m.call(g.call(*a)) }; end
end
