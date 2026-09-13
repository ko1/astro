# repro: `binding` reached as a real method (send / explicit receiver / Kernel.binding)
class Demo
  def get_with_send
    a = 1
    [send(:binding), __LINE__]
  end
end
def top
  z = 42
  Kernel.binding
end
b, line = Demo.new.get_with_send
p b.class
p b.local_variables.sort
p b.eval("a")
p b.eval("__LINE__")
p b.eval("self").class
p top.local_variable_get(:z)
m = Object.new
p Object.instance_method(:class).bind(m) && true
p eval('self', m.send(:binding)) == self
p Kernel.private_instance_methods(false).include?(:binding)
p Kernel.public_methods(false).include?(:binding)
