# Integer/Float/Rational #<=> use #coerce for a coercible object; else nil. vs ruby.
class C; def coerce(o); [o.to_f, 2.0]; end; end
p(5 <=> C.new)
p(5.0 <=> C.new)
p(Rational(1, 2) <=> C.new)
p(5 <=> "x")
p(5 <=> Object.new)
p(3 <=> 5); p(5.0 <=> 5); p(Rational(3, 2) <=> 1)
