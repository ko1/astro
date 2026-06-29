require "set"
s = Set[1, 2, 3]
d = s.dup
p d.equal?(s)
p d.to_a.sort
d << 4
p s.to_a.sort
p d.to_a.sort
class MySet < Set; end
p MySet[1, 2].dup.class
p Set[1, 2].clone.to_a.sort
