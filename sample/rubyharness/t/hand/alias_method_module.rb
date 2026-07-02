# alias_method inside a module can alias an Object/Kernel instance method
# (koruby keeps those on Object). vs ruby.
class Object
  def base_greeting; "hi"; end
end
module Mod
  alias_method :mod_greeting, :base_greeting
end
class Consumer
  include Mod
end
p Consumer.new.mod_greeting

# ordinary same-class alias still works
class Calc
  def add(a, b); a + b; end
  alias_method :plus, :add
end
p Calc.new.plus(2, 3)

# aliasing a missing method raises NameError
begin
  Calc.class_eval { alias_method :x, :nonexistent }
rescue NameError => e
  p e.class
end
