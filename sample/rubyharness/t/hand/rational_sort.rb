p [Rational(1,2), Rational(1,3), Rational(1,4)].sort
p [Rational(1,2), Rational(1,3)].min
p [Rational(1,2), Rational(1,3)].max
p [1.5, Rational(1,2), 2].sort
p [Rational(3,2), Rational(1,4), Rational(5,6)].sort_by { |r| r }
p [Rational(1,2), Rational(1,3), Rational(1,4)].min(2)
p [Rational(1,2), Rational(1,3), Rational(1,4)].max(2)
p [Rational(1,2), 0.4, 1, Rational(3,4)].sort
p [Rational(-1,2), Rational(1,2), 0].sort
p((Rational(1,3)..Rational(1,2)).include?(Rational(2,5)))
p [Rational(7,3), Rational(7,3)].uniq.size
p [Rational(22,7), Rational(355,113), Math::PI].sort_by(&:to_f).map { |x| x.to_f.round(4) }
p [Rational(1,2), Rational(2,4)].uniq.size
