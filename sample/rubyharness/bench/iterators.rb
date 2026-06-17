s = 0
200_000.times do |j|
  5.upto(100) { |x| s += x }
  100.downto(50) { |x| s -= x }
  0.step(200, 4) { |x| s += x }
end
p s
