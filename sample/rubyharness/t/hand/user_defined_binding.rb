# `binding` is an ordinary Kernel method: a class that defines its own wins
# (rubocop-ast has `attr_reader :binding`).  The hidden binding eval(str) is
# lowered with must NOT go through it.
class C
  attr_reader :binding
  def initialize = @binding = "MINE"
  def get = binding
  def get2 = self.binding
end
c = C.new
p [c.get, c.get2]

class D
  def binding = "D-binding"
  def use = binding
  def use_eval(x) = eval("x + 1")     # the hidden binding still works
end
p D.new.use
p D.new.use_eval(41)

def top_binding = binding
p top_binding.class
p top_binding.local_variables.class
class E
  def m
    y = 7
    eval("y * 2")
  end
end
p E.new.m
p [Kernel.private_method_defined?(:binding), Object.new.respond_to?(:binding, true)]
