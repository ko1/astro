# Range#cover? / #=== dispatch #<=> for non-builtin bounds (custom Comparable,
# Bignum), and Range#=== uses cover? (not succ-based include?). vs ruby.
class C
  include Comparable
  attr_reader :v
  def initialize(v); @v = v; end
  def <=>(o); v <=> o.v; end
end
r = C.new(1)..C.new(5)
p r.cover?(C.new(3))
p r.cover?(C.new(9))
p r.cover?(C.new(0))
p (C.new(1)..C.new(9)).cover?(C.new(3)..C.new(7))
p ((2**70)..(2**80)).cover?(2**75)
p ((2**70)..(2**80)).cover?(2**90)
# === uses cover?, so multi-char strings differ from succ-based include?
p(("a".."z") === "cc")
p(("a".."z").include?("cc"))
p((1..10) === 5.5)
p((1..10).include?(5.5))
case C.new(3)
when C.new(1)..C.new(5) then p "in"
else p "out"
end
case 5.5
when 1..10 then p "num-in"
end
