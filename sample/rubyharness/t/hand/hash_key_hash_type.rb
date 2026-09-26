# A Hash / Set key whose #hash answers a non-Integer raises TypeError, and a
# Module's own `def self.hash` is called (pry's completion rescues that TypeError).
require 'set'
m = Module.new { def self.hash; end }
p((Set[Object].include?(m) rescue [$!.class, $!.message]))
p(({Object => 1}[m] rescue [$!.class, $!.message]))
class HN; def hash = nil; end
p(({1 => 2}[HN.new] rescue [$!.class, $!.message]))
class HS; def hash = "x"; end
p(({1 => 2}[HS.new] rescue [$!.class, $!.message]))
class HB; def hash = 2**70; end
p({HB.new => 1}.size)
k = Module.new { def self.hash = 42; def self.eql?(o) = o.equal?(self) }
h = {k => :v}; p h[k]
