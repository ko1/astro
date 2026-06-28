class R; def to_r; Rational(3, 4); end; end
p Rational(R.new)
p Rational(5)
p Rational(1, 2)
p Rational(R.new) + Rational(1, 4)
p Rational(R.new).class
