# upto / downto / step / Range iteration across the Fixnum/Bignum edge: values
# must widen into Bignums, never wrap around or read a Bignum as a Fixnum.
m = -(2 ** 62); x = 2 ** 62 - 1
m.downto(m - 2) { |i| p i }
x.upto(x + 2) { |i| p i }
p x.upto(x + 2).to_a, m.downto(m - 2).to_a, (x + 5).downto(x + 3).to_a
p m.step(m - 2, -1).to_a, x.step(x + 4, 2).to_a, 1.step(2 ** 64, 2 ** 63).to_a
m.step(m - 2, -1) { |i| p i }
(m - 2..m).each { |i| p i }
p (m - 2...m).to_a, (x..x + 2).to_a, (x..x + 2).map { |i| i - x }
p (2 ** 70).upto(2 ** 70 + 1).map { |i| i - 2 ** 70 }
p 5.upto(4).to_a, (2 ** 64).downto(2 ** 64 + 1).to_a
p 4611686018427387903.succ, -4611686018427387904 - 1, x.upto(x.to_f + 1).to_a.size
