# #<X:0x…> of a class / object / module / proc does not change when the GC moves it
c = Class.new; o = Object.new; m = Module.new; pr = proc {}
a = [c.inspect, o.inspect, m.inspect, o.to_s, c.new.singleton_class.inspect.size, pr.inspect.size]
x = []; 20000.times { x << "s" * 10 }; GC.start; GC.start
b = [c.inspect, o.inspect, m.inspect, o.to_s, c.new.singleton_class.inspect.size, pr.inspect.size]
p a == b
p c.inspect =~ /\A#<Class:0x\h{16}>\z/, o.inspect =~ /\A#<Object:0x\h{16}>\z/
p Object.new.inspect != Object.new.inspect
