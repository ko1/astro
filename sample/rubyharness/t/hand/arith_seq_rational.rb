# ArithmeticSequence over Rational / BigDecimal (net-imap: ((0.0r..1.0r) % 0.1r).to_a); Numeric#step without a
# block is one; inspect prints the receiver with #to_s; step(0, nil) keeps the nil
a = (0r..1r) % 0.25r; p a, a.to_a, a.size, a.first(2), a.last, (1r.step(2r, 0.5r)).to_a, ((0r...1r) % 0.5r).to_a, ((0r...1r) % 0.5r).size, (1r.step(0r, -0.5r)).to_a, 1r.step(nil, 0.5r).first(3), 1r.step(nil, 0.5r).size, (1r.step(2, 0.3r)).size, ((1..2r) % 0.5r).to_a
p a.first, ((0.0r..1.0r) % 0.1r).to_a.size
x = []; ((0r..1r) % 0.5r).each { |v| x << v }; p x
require "bigdecimal"; p (BigDecimal("0.1").step(BigDecimal("0.3"), BigDecimal("0.1"))).to_a.map(&:to_s)
begin; ((1r..2r) % 0r).to_a; rescue => e; p e.class; end
p 1r.step, 1r.step(2), 1r.step(2, 0.5r), (1/2r).step(0)
p 1r.step(2, "x").class
p (("a".."e").step(2)), ((1.0..2.0).step(0.5)), 1.step(3), ((1..2) % 1), (1..).step(2), 1.0.step
