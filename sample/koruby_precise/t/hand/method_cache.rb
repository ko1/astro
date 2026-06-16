# method-dispatch heavy: exercises the VM method cache (hit/poly/redef/inherit)
class Animal
  def initialize(n); @n = n; end
  def speak; "..."; end
  def name; @n; end
  def greet; "#{name} says #{speak}"; end
end
class Dog < Animal
  def speak; "woof"; end
end
class Cat < Animal
  def speak; "meow"; end
end

zoo = []
3.times { |i| zoo << Dog.new("D#{i}") << Cat.new("C#{i}") << Animal.new("A#{i}") }
# monomorphic-per-iteration + polymorphic across the array (cache thrash)
p zoo.map { |a| a.greet }

# tight loop on one receiver (warm cache)
d = Dog.new("Rex")
acc = ""
100.times { acc = d.speak }
p acc

# method redefinition must invalidate the cache (serial bump)
class Dog
  def speak; "WOOF!"; end
end
p d.speak

# inherited method via super chain, called in a loop
class Loud < Dog
  def speak; super + "!!!"; end
end
l = Loud.new("Max")
p (0...3).map { l.speak }

# self-call dispatch (no explicit receiver) in a loop
class Counter
  def initialize; @c = 0; end
  def tick; bump; bump; @c; end
  def bump; @c += 1; end
end
c = Counter.new
p (0...4).map { c.tick }
