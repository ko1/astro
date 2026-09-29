# Date#<=> with something neither Numeric nor Date goes through other.coerce(self), as CRuby's cmp_gen
# (lobsters: TimeWithZone <= 1.hour went via DateTime <=> Duration).  Run under CRuby and koruby and diff.
require "date"
o = Object.new; def o.coerce(x) = [1, 2]
q = Object.new; def q.coerce(x) = :bad
r = Object.new; def r.ajd = 0
p Date.today <=> o, Date.today <=> r, Date.today <=> 3, DateTime.now <=> Date.today
p Date.new(2020,1,1) <=> Date.new(2020,1,2), (Date.today < o rescue $!.class)
q = Object.new; def q.coerce(x) = :bad
p((Date.today <=> q rescue [$!.class, $!.message]))
q2 = Object.new; def q2.coerce(x) = nil
p Date.today <=> q2
