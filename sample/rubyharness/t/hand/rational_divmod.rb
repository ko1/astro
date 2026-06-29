p Rational(7, 2).divmod(Rational(1, 1))
p Rational(7, 2).divmod(2)
p Rational(-7, 2).divmod(Rational(1, 1))
def t; yield; rescue ZeroDivisionError; "ZDE"; end
p t { Rational(7, 2).divmod(Rational(0, 1)) }
