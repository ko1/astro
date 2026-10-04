# GC.disable stops allocation-driven collection (objects stay, GC.count does not move); GC.start still collects
g = Struct.new(:i)
GC.disable
c0 = GC.count
200_000.times { |i| g.new(i); "x" * 50 }
p GC.count == c0
p ObjectSpace.each_object(g).count >= 200_000
p GC.disable, GC.enable, GC.enable
GC.start
p GC.count > c0
