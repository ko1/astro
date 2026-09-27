# Random.rand follows Random#rand (0 / negative / nil are ArgumentError, 0.0 is a plain
# draw, a Float bound keeps a Float, NaN is EDOM); a non-number is a TypeError for
# every entry point; draws above 2**32 and Bignum bounds / spans take CRuby's words
# in CRuby's order (same numbers for the same seed); Kernel#rand does not send the
# user-reopenable Float#to_i / Integer#abs.
srand(1234)
%w[Random.rand(0.0) Random.rand(0) Random.rand(-1.5) Random.rand(-3) Random.rand(1.5).class Random.rand(3).class Random.rand(nil).class Random.rand(1..2).class Random.rand(2**70).class Random.rand(Float::NAN) Random.new(1).rand(0.0) Random.new(1).rand(nil) Random.rand(false) Random.rand("3") rand("3") rand(Float::NAN) rand(Float::INFINITY) rand(1e30).class rand(-2.5).class rand(0.9).class].each do |x|
  v = begin; eval(x); rescue Exception => e; [e.class, e.message]; end
  puts "#{x} => #{v.inspect}"
end
srand(1); p rand(2**100); srand(1); p rand(-(2**100))
p Random.new(1).rand(2**100), Random.new(1).rand(2**64), Random.new(1).rand(2**40), Random.new(1).rand(10**30), Random.new(7).rand(2**32 + 1), Random.new(7).rand(2**33)
r = Random.new(42); p 20.times.map { r.rand(3 * 2**62) }.sum, 20.times.map { r.rand(10**40 + 7) }.sum, 20.times.map { r.rand(2**64 - 1) }.sum
p Random.new(5).rand(2**64..2**65), Random.new(5).rand(2**40..2**41), Random.new(5).bytes(5).bytes
e = 'r=Random.new(3); [r.rand(2**64...2**64+1), (r.rand(2**65..2**64) rescue $!.class), rand(2**65..2**64), rand(2**64..2**64).class, Random.new(9).rand(-(2**70)..-(2**69))]'
p eval(e)
class Float; def to_i = 0; end
srand(1); x = rand(1.5); p x.class, x
class Integer; def abs = 1; end
srand(1); p rand(-(2**100))
