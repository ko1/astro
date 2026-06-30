# Integer#pow(neg, mod) RangeError; Integer#remainder(0.0) ZeroDivisionError;
# Hash#merge retains the receiver's default. vs ruby.
begin; 2.pow(-2, 5); rescue => e; p e.class; end
p 2.pow(3, 5)
p 2.pow(10, 1000)
begin; 5.remainder(0.0); rescue => e; p e.class; end
begin; (10**40).remainder(0.0); rescue => e; p e.class; end
p 13.remainder(4.0)
p (10**40).remainder(7)
h = Hash.new(99); m = h.merge({ a: 2 }); p m[:missing]; p m[:a]
h2 = Hash.new { |hh, k| k * 2 }; m2 = h2.merge({ a: 1 }); p m2[5]
h3 = { x: 1 }; p h3.merge({ y: 2 })[:nope]  # no default → nil
