p (1..Float::INFINITY).first(3)
p (1..Float::INFINITY).take(5)
p (5..Float::INFINITY).first(2)
p (1..).first(3)
p (1..10).first(3)
p (1..Float::INFINITY).first
p (1..Float::INFINITY).lazy.map { |x| x*2 }.first(3)
p (0..Float::INFINITY).step(2).first(4)
