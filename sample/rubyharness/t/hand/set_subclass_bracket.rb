require "set"
class MySet < Set; end
s = MySet[1, 2, 2, 3]
p s.class
p s.size
p s.include?(2)
p s.map { |x| x }.sort
t = Set[1, 2, 3]
p t.class
