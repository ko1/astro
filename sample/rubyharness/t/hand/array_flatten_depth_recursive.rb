# Depth-limited Array#flatten on a recursive array does NOT raise (the depth
# bounds recursion); only unlimited flatten raises. vs ruby. (Check structure,
# not inspect, since the recursion-marker display differs.)
c = [1, 2]; c << c
r1 = c.flatten(1); p r1.size; p r1[0, 4]; p r1.last.equal?(c)
r2 = c.flatten(2); p r2.size; p r2.last.equal?(c)
begin; c.flatten; rescue => e; p e.class; end
d = [1, [2, [3, [4]]]]
p d.flatten(1); p d.flatten(2); p d.flatten
