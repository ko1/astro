# a == b / a != b of user objects: BasicObject's identity default answers without the send; every override still runs
class Plain; end
class Val; attr_reader :v; def initialize(v) = @v = v; def ==(o) = o.is_a?(Val) && o.v == v; end
class Cmp; include Comparable; attr_reader :v; def initialize(v) = @v = v; def <=>(o) = v <=> o.v; end
class OnlyNe; def !=(o) = :custom_ne; end
module EqMod; def ==(o) = :mod_eq; end
class Late; end
S = Struct.new(:a)
a, b = Plain.new, Plain.new
2.times do
  p a == b, a != b, a == a, a != a, Val.new(1) == Val.new(1), Val.new(1) != Val.new(2), Val.new(1) != Val.new(1)
  p Cmp.new(1) == Cmp.new(1), Cmp.new(1) != Cmp.new(2), OnlyNe.new != 1, OnlyNe.new == 1, S.new(1) == S.new(1), S.new(1) != S.new(1)
  p a == 1, a != nil, a == "x"
end
l1, l2 = Late.new, Late.new
p l1 == l2, l1 != l2
class Late; include EqMod; end
p l1 == l2, l1 != l2
class Late; def ==(o) = :late_eq; end
p l1 == l2, l1 != l2
def l1.==(o) = :single
p l1 == l2, l1 != l2, l2 == l1
3.times { |i| GC.start if i == 1; x, y = Plain.new, Plain.new; p x == y, x != y }
module R; refine Plain do; def ==(o) = :refined; end; end
p a == b
using R
p a == b
