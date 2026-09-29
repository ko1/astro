# Numerics koruby does not know (BigDecimal from a C extension, here a Ruby
# Numeric): Complex parts, #quo/#fdiv through coerce, plus Float#divmod by
# ±Infinity and Enumerable over endless / Bignum-bounded ranges.
class Dec < Numeric
  attr_reader :v
  def initialize(v) = @v = v.to_r
  def coerce(o) = [Dec.new(o), self]
  def +(o) = Dec.new(v + (o.is_a?(Dec) ? o.v : o))
  def -(o) = Dec.new(v - (o.is_a?(Dec) ? o.v : o))
  def *(o) = Dec.new(v * (o.is_a?(Dec) ? o.v : o))
  def /(o) = Dec.new(v / (o.is_a?(Dec) ? o.v : o))
  def quo(o) = self / o
  def <=>(o) = v <=> (o.is_a?(Dec) ? o.v : o)
  def abs = Dec.new(v.abs)
  def negative? = v.negative?
  def to_f = v.to_f
  def real? = true
  def to_s = "D(#{v})"
  def inspect = "D<#{v}>"
end
d = Dec.new(3)
def t(label) = (puts "#{label}: #{yield.inspect}" rescue puts "#{label}: #{$!.class}: #{$!.message}")
t(:cdiv) { Complex(1, 2) / d }
t(:ddivc) { Complex(d, 1) / Complex(1, 2) }
t(:cmul) { Complex(d, d) * Complex(1, 2) }
t(:cadd) { Complex(d, 0) + Complex(1, 2) }
t(:intmul) { 2 * Complex(d, 1) }
t(:fltmul) { 1.5 * Complex(d, 1) }
t(:abs) { Complex(d, 4).abs }
t(:to_s) { Complex(d, 1).to_s }
puts Complex(d, 1)
p Complex(d, 1), [Complex(d, 1), Complex(1, 2)]
t(:int_quo) { 1.quo(d) }
t(:big_quo) { (2**70).quo(d) }
t(:flt_quo) { 1.5.quo(d) }
t(:flt_fdiv) { 1.5.fdiv(d) }
t(:divmod_inf) { [-4.2, 0.0, -0.0, 4.2].map { |x| [x.divmod(Float::INFINITY), x.divmod(-Float::INFINITY)] } }
t(:endless) { [(1..).find { _1 > 3 }, (1..).find_index { _1 > 5 }, (1..).each_slice(2).first(2), (1..).each_cons(2).first(2),
                (1..).each_with_index.first(2), (1..).take_while { _1 < 4 }, (1..).any? { _1 > 3 }, (1..).all? { _1 < 3 },
                (1..).none? { _1 > 3 }, (1..).detect { _1 * _1 > 50 }, (1..).find(-> { :none }) { _1 > 2 }] }
t(:endless_slice_break) { (1..).each_slice(3) { |s| break s if s.sum > 20 } }
t(:bsearch_big) { [(1..2**70).bsearch { _1 > 100 }, (1...2**70).bsearch { _1 >= 2**69 } == 2**69, (2**64..).bsearch { _1 > 2**65 },
                   (..2**64).bsearch { _1 > -5 }, (1..2**70).bsearch { 2**40 <=> _1 }, (1..2**70).bsearch { false }] }
t(:bsearch_big_bad) { (1..2**70).bsearch { "x" } }
