class Animal; def speak; "..."; end; end
class Dog < Animal; def speak; "Woof"; end; end
um = Animal.instance_method(:speak)
p um.bind(Dog.new).call
p um.bind_call(Dog.new)
p Dog.new.method(:speak).call
p Dog.new.method(:speak).unbind.bind(Dog.new).call
class Base; def greet(n); "Hi #{n}"; end; end
class Sub < Base; def greet(n); "Yo #{n}"; end; end
p Base.instance_method(:greet).bind(Sub.new).call("Bob")
p Sub.new.method(:greet).call("Bob")
p 5.method(:+).call(3)
p [1,2,3].method(:map).call { |x| x * 2 }
module M; def hi; "M#hi"; end; end
class WithM; include M; def hi; "WithM#hi"; end; end
p M.instance_method(:hi).bind(WithM.new).call
p WithM.new.method(:hi).call
class Greeter; def hello(name); "Hello, #{name}\!"; end; end
unbound = Greeter.instance_method(:hello)
p unbound.bind_call(Greeter.new, "World")
p Animal.instance_method(:speak).owner
p Dog.new.method(:speak).owner
class C1; def m; 1; end; end
class C2 < C1; def m; 2; end; end
class C3 < C2; def m; 3; end; end
p C1.instance_method(:m).bind(C3.new).call
p C2.instance_method(:m).bind(C3.new).call
p Integer.instance_method(:to_s).bind(255).call(16)
p String.instance_method(:upcase).bind("hi").call
p Integer.instance_method(:+).bind(10).call(5)
