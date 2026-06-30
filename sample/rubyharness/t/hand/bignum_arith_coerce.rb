# Bignum arithmetic with non-Integer raises TypeError (coerce), not NoMethodError.
# Bignum % Float yields a Float modulo. vs ruby.
big = 10**40
begin; big + "10"; rescue => e; p [:plus, e.class]; end
begin; big - "10"; rescue => e; p [:minus, e.class]; end
begin; big * "10"; rescue => e; p [:mul, e.class]; end
begin; big / "10"; rescue => e; p [:div, e.class]; end
begin; big % "10"; rescue => e; p [:mod, e.class]; end
begin; big - :symbol; rescue => e; p [:minus_sym, e.class]; end
p (big % 3)
p (big % 2.5).class
p ((big % 2.5) >= 0)
begin; big % 0.0; rescue => e; p [:modzero, e.class]; end
# coerce protocol still works (object with coerce)
class C5; def coerce(o); [o, 100]; end; end
p (10 / C5.new)
p (10 - C5.new)
