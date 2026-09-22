# BasicObject does not inherit Kernel, so the global functions (puts/gets/…)
# are not among its private instance methods — and every listed name must be
# resolvable with #instance_method (pry's find-method walks every constant).
def chk(m, label)
  names = Module.instance_method(:private_instance_methods).bind(m).call(true)
  bad = names.reject { |n| (Module.instance_method(:instance_method).bind(m).call(n) rescue nil) }
  puts "#{label}: bad=#{bad.first(4).inspect}"
end
chk(Object, "Object")
chk(Kernel, "Kernel")
chk(BasicObject, "BasicObject")
chk(Class.new(BasicObject), "BasicObject subclass")
chk(Object.new.singleton_class, "obj singleton")
chk(Module.new, "anon module")
p BasicObject.private_instance_methods(true).sort
p Class.new(BasicObject).private_instance_methods(true).sort
require 'delegate'
p Delegator.private_instance_methods(true).include?(:puts)
p [Delegator.public_api.include?(:send), Delegator.superclass]
