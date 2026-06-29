require "set"
s = Set[1, 2, 3, 4]
r = s.subtract([2, 4])
p s.to_a.sort
p r.equal?(s)
t = Set[1, 2, 3]
t.subtract(Set[2])
p t.to_a.sort
u = Set[1, 2, 3]
u.subtract([])
p u.to_a.sort
