p (1..Float::INFINITY).lazy.map { |x| x * 2 }.take_while { |x| x < 10 }.force
p (1..).lazy.take_while { |x| x < 5 }.to_a
p (1..Float::INFINITY).lazy.select(&:even?).take_while { |x| x < 20 }.force
p (1..).lazy.map { |x| x ** 2 }.take(5).force
p (1..).lazy.take_while { |x| x < 100 }.select(&:even?).first(3)
p (1..Float::INFINITY).lazy.take(3).force
p (1..10).lazy.take_while { |x| x < 5 }.force
p (1..).lazy.drop_while { |x| x < 5 }.take(3).to_a
p (1..).lazy.map { |x| x * x }.take_while { |x| x < 50 }.to_a
p (2..).lazy.select { |n| (2...n).none? { |d| n % d == 0 } }.first(5)
