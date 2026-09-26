# A class whose #inspect is Ruby-defined (own, inherited or extended) prints through it in p / %p / containers;
# exception inspect names the full class path.
class C; def self.inspect = "CC"; def self.to_s = "CS"; def inspect = "ci"; end
class D < C; end
module Named; def inspect = "NM"; end
class E; extend Named; end
class F; def self.to_s = "only_to_s"; end
p format("%p %s", C, C), "#{C}", [C, D, E, F].inspect, {C => 1}.inspect, C, D, [C.new]
class G; def self.inspect = raise("boom"); end
p(([G].inspect rescue $!.message))
module A; class B < StandardError; end; end; p A::B.new("x"), A::B.new, A::B.new(""), [A::B.new("y")], Errno::ENOENT.new
