# case/when over many Fixnum literals is dispatched by value; results must equal the linear chain.
def f(x)
  case x
  when 1 then :one
  when 2, 3 then :two_three
  when -5 then :minus_five
  when 3 then :dup_three
  when 10, 11, 12 then :ten
  when 100 then :hundred
  when 4611686018427387903 then :fixmax
  when -4611686018427387904 then :fixmin
  when 7 then :seven
  when 0 then :zero
  else :other
  end
end
class Eq1; def ==(o) = o == 7; end
p [-6, -5, 0, 1, 2, 3, 4, 7, 10, 11, 12, 13, 99, 100, 101, 4611686018427387903, -4611686018427387904, 2**62, 2**64].map { f(_1) }
p [1.0, 3.0, 7r, Complex(10, 0), 100.0, Eq1.new, "1", nil, :a].map { f(_1) }
def g(x) = (case x; when 1, 2, 3, 4, 5, 6, 7, 8 then x * 2; end)
p (0..9).map { g(_1) }
h = ->(x) { case x when 1 then (return :early) when 2, 3, 4, 5, 6, 7, 8, 9 then :late end; :tail }
p (1..10).map { h.(_1) }
def k(x) = (case x; when 1, 2, 3, 4, 5, 6, 7, 8; end)
p k(3), k(0)
