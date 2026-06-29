require "set"
class S < Set; end
s = S.new([1, 2, 2, 3])
p s.size
p s.class
p s.include?(2)
p s.to_a.sort
s2 = S.new
p s2.size
p s2.class
