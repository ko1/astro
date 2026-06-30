p 1.send(:+, Rational(1, 2))
p [1,2,3].sum { |x| x.to_r }
p [Rational(1,2), Rational(1,3)].reduce(:+)
p 1.send(:-, Rational(1, 4))
p 2.send(:*, Rational(3, 4))
p Rational(1,2).send(:+, 1)
p 1.5 + Rational(1, 2)
p Rational(1,2) + 1.5
p [Rational(1,2), 1, Rational(1,4)].sum
p [1, 2, 3].sum { |x| Rational(x, 2) }
p 1.send(:+, Complex(2, 3))
p [Complex(1,1), Complex(2,2)].reduce(:+)
p 5.send(:+, 3)
p [1,2,3].reduce(:+)
p (1..5).sum { |x| Rational(x, 1) }
p [Rational(1,3)] * 3
p 10.send(:/, Rational(2, 1)) rescue p "div"
p [1,2,3,4].sum(Rational(0)) { |x| Rational(x, 2) }
