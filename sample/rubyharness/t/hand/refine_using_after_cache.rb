# A call site cached while no refinement set was active must see a refinement
# that `using` (or a Proc#refined copy) brings in later, even when refinements
# were already on elsewhere; and a builtin class constant that was removed and
# assigned back still counts as that builtin (subclassing, dispatch).
class C
  attr_reader :x
  def initialize = @x = :base
end
module R0; refine Array do; def unrelated = :r0; end; end
module R1; refine C do; def x = :refined; end; end
proc {}.refined(R0)
o = C.new
2.times do |i|
  p o.x
  using R1 if i == 0
end
class D; def y = :base; end
module R2; refine D do; def y = :ref; end; end
pr = proc { |d| d.y }
d = D.new
p pr.call(d)
rp = pr.refined(R2)
p rp.call(d), pr.call(d), rp.call(d)

saved = String
Object.send(:remove_const, :String)
String = saved
class X < String; end
x = X.new("a")
p [x.class, x.to_s, x.upcase]
saved = Array
Object.send(:remove_const, :Array)
Array = saved
p [1, 2].map { _1 * 2 }, Class.new(Array).new([1]).size
