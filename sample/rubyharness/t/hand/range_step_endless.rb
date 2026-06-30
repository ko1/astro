# Range#step over endless ranges: iterate from begin upward, block breaks. vs ruby.
a = []; (-2..).step { |x| break if x > 2; a << x }; p a
b = []; (-5..).step(2) { |x| break if x > 3; b << x }; p b
c = []; (1..).step(0.5) { |x| break if x > 3; c << x }; p c
d = []; (1.0..).step { |x| break if x > 3; d << x }; p d
e = []; (1.0..).step(0.5) { |x| break if x > 2.5; e << x }; p e
f = []; (1.0..).step(2) { |x| break if x > 9; f << x }; p f
g = []; (0...).step(3) { |x| break if x >= 12; g << x }; p g
# bounded still correct
p((1..10).step(3).to_a)
p((1.0..2.0).step(0.5).to_a)
