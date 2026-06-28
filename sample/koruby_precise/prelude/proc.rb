# Proc#curry — partial application.
class Proc
  def curry(n = (arity < 0 ? -arity - 1 : arity))
    acc = nil
    acc = ->(got) { got.length >= n ? call(*got) : ->(*more) { acc.call(got + more) } }
    acc.call([])
  end
end
