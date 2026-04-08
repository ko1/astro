# Polymorphic method dispatch (inheritance chain)
class Base
  def value = 1
end
class Mid < Base
  def value = 2
end
class Leaf < Mid
  def value = 3
end

objs = [Base.new, Mid.new, Leaf.new]

sum = 0
i = 0
while i < 24_000_000
  sum += objs[i % 3].value
  i += 1
end
p(sum)
