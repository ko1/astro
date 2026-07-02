# module_function: named form and no-arg mode. Instance methods become private,
# public copies live on the module singleton. Returns arg/args. vs ruby.
module M
  def foo; "foo"; end
  module_function :foo
end
p M.foo
p M.private_instance_methods(false).include?(:foo)
p M.public_instance_methods(false).include?(:foo)

module N
  module_function
  def a; "a"; end
  def b; "b"; end
end
p [N.a, N.b]
p N.private_instance_methods(false).sort
p N.public_instance_methods(false)

module R
  def x; end; def y; end
  RET1 = module_function(:x)
  RET2 = module_function(:x, :y)
end
p R::RET1
p R::RET2

# a module_function method is callable via send from an includer (private)
module Mix
  module_function
  def helper; "helped"; end
end
class Uses; include Mix; end
p Uses.new.send(:helper)
