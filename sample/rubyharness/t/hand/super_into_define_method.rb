# super resolving to a define_method'd method runs its Proc body (was a SIGBUS:
# korb_super invoked a DM method as if it were ISEQ). vs ruby.
class Base
  define_method(:greet) { |name| "hello #{name}" }
end
module Prep
  def greet(name); "[" + super + "]"; end
end
class Base
  prepend Prep
end
p Base.new.greet("world")

# subclass regular method super-ing into a define_method'd parent method
class Parent
  define_method(:val) { 41 }
end
class Child < Parent
  def val; super + 1; end
end
p Child.new.val
