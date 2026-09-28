$log = []
class Module
  prepend(Module.new { def const_added(n) = ($log << [self.inspect, n]; super) })
end
module TopM; end
class TopC; end
TOPV = 1
module TopM; class Inner; end; module InnerM; end; K = 2; end
Object.const_set(:DYN, 3)
TopM.const_set(:DYN2, 4)
module TopM; end   # reopen: no event
p $log
