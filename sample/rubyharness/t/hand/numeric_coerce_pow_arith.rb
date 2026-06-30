# Float#**, Rational arith/**, Integer#divmod use #coerce for an Object. vs ruby.
class D; def coerce(o); [10, 3]; end; end
p(5.0 ** D.new)
p(Rational(1, 2) + D.new)
p(Rational(1, 2) - D.new)
p(Rational(3, 2) * D.new)
p(Rational(3, 2) ** D.new)
p(10.divmod(D.new))
p(2.0 ** 10)
p(Rational(1, 2) + Rational(1, 3))
p(10.divmod(3))
begin; 5.0 ** Object.new; rescue => e; p e.class; end
