r = []; (1..Float::INFINITY).each { |x| break if x > 4; r << x }; p r
r2 = []; (1..).each { |x| break if x > 3; r2 << x }; p r2
sum = 0; (1..).each { |x| break if sum > 10; sum += x }; p sum
found = nil; (10..).each { |x| if x % 7 == 0; found = x; break; end }; p found
collected = []; (100..).each { |x| collected << x; break if collected.size == 3 }; p collected
p (1..Float::INFINITY).first(3)
p (5..).take(3)
p (1..).first(5)
n = 0; (1..10).each { |x| n += x }; p n
p (1..3).each { |x| }
primes = []; (2..).each { |x| primes << x if (2...x).none? { |d| x % d == 0 }; break if primes.size == 5 }; p primes
