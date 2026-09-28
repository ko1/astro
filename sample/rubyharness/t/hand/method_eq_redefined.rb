m = Module.new
m.class_eval { def speed = 0 }
a = m.instance_method(:speed)
m.class_eval { def speed = 1 }
b = m.instance_method(:speed)
p a == b, a.source_location[1], b.source_location[1]
m.class_eval "def speed(*a) = 2"
c = m.instance_method(:speed)
p b == c, c.source_location[1]
m.class_eval { def speed = 3 }
d = m.instance_method(:speed)
p c == d, d.source_location[1]
m = Module.new
2.times do |round|
  old_methods = m.instance_methods.to_h { |n| [n, m.instance_method(n)] }
  m.class_eval do
    if round == 0 then def speed = 0 else def speed = 'green' end
  end
  $cur = m.instance_method(:speed) if round == 1
  now = m.instance_methods.to_h { |n| [n, m.instance_method(n)] }
  new_methods = now.reject { |name, meth| old_methods[name] == meth }
  p [round, new_methods.keys]
  new_methods.each_key do |name|
    m.class_eval <<-E, __FILE__, __LINE__ + 1
      alias_method :"__on_#{name}__", :#{name}
      def #{name}(*args, &block) = [:wrapped, __send__(:"__on_#{name}__", *args, &block)]
    E
  end
end
o = Object.new.extend(m)
p o.speed, m.instance_method(:__on_speed__) == $cur
