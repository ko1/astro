# L0: ranges
p (1..5).to_a
p (1...5).to_a
p ('a'..'e').to_a
p (1..5).include?(3)
p (1..5).include?(6)
p (1..5).cover?(3)
p (1..10).min
p (1..10).max
p (1..10).size
p (1..5).sum
p (1..5).first
p (1..5).last
p (1..5).first(2)
p (1..5).last(2)
p (1..5) === 3
p (0...3).to_a
p (5..1).to_a
p (1..5).step(2).to_a
p (1.0..2.0).step(0.5).to_a
p (1..5).each_slice(2).to_a
