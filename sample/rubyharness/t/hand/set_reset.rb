require "set"
s = Set.new([[1], [2]])
a = s.to_a[0]
a << 9
# NOTE: before #reset, CRuby answers false here (the mutated element sits in a
# stale bucket of the backing Hash).  koruby's Set is an element ARRAY, so the
# linear scan finds it — a divergence #reset cannot paper over.  Not asserted.
p s.reset.equal?(s)
p s.include?([1, 9])
p s.to_a
d = Set.new([[1], [1, 9]])
d.to_a[0] << 9
p d.size
d.reset
p d.size, d.to_a
fs = Set.new([1]).freeze
begin; fs.reset; rescue => e; p [e.class, e.message]; end
p Set.new.reset.to_a
p Set.instance_methods.include?(:reset)
