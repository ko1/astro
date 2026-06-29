require "set"
p Set[1, 2, Set[3, 4], Set[5, Set[6]]].flatten.to_a.sort
p Set[1, 2, 3].flatten.to_a.sort
s = Set[1, 2]
s2 = Set[s, 3]
def t; yield; rescue ArgumentError; "AE"; end
r = Set[1, Set[2]]
r << r
p t { r.flatten }
