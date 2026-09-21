# `rand(range)` works over any pair that answers #- and #+, not just numbers:
# CRuby draws over `hi - lo` and answers `lo + n`.  koruby raised
# "bad value for range" (ffaker's test_identification_fi does
# `rand(Date.civil(1700)..Date.civil(1800))`), and it decided Float-vs-Integer
# from the BOUNDS rather than from the span, so an exact Rational range came
# back as a Float.
require 'date'

[1, 5, 12345].each do |seed|
  r = Random.new(seed)
  p [seed, r.rand(Date.civil(1700)..Date.civil(1800)).to_s, r.rand]
  r = Random.new(seed)
  p [seed, r.rand(Date.civil(1700)...Date.civil(1800)).to_s, r.rand]
  r = Random.new(seed)
  p [seed, r.rand(Rational(1, 2)..Rational(5, 2)), r.rand]
  r = Random.new(seed)
  p [seed, r.rand(Rational(1, 2)...Rational(5, 2)), r.rand]
  r = Random.new(seed)
  p [seed, r.rand((10**30)..(10**30 + 5)), r.rand]
end
srand(9)
p rand(Date.civil(2000)..Date.civil(2001)).to_s

# a Comparable of one's own
class Meas
  include Comparable
  attr_reader :v
  def initialize(v)
    @v = v
  end
  def -(o) = @v - o.v
  def +(n) = Meas.new(@v + n)
  def <=>(o) = @v <=> o.v
  def to_s = "M(#{@v})"
end
p [Random.new(3).rand(Meas.new(0)..Meas.new(10)).to_s,
   Random.new(3).rand(Meas.new(0)...Meas.new(10)).to_s,
   Random.new(3).rand(Meas.new(4)..Meas.new(4)).to_s]

# a zero-width object range draws nothing, so the next value is the first one
r = Random.new(1); r.rand(Date.civil(2000)..Date.civil(2000)); p r.rand
r = Random.new(1); p r.rand
# a reversed object range
p (Random.new(1).rand(Date.civil(2001)..Date.civil(2000)) rescue [$!.class, $!.message])
# a Float bound still draws a Float
p [Random.new(1).rand(1..2.0), Random.new(1).rand(1.0..2)]
