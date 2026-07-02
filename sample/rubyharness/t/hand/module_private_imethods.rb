# a bare module's private_instance_methods excludes Kernel builtins (modules
# don't inherit Kernel); a class still includes them. vs ruby.
module M
  private
  def secret; end
end
p M.private_instance_methods
p M.private_instance_methods(false)
class C
  private
  def hidden; end
end
p C.private_instance_methods(false)
p C.private_instance_methods.include?(:puts)   # class inherits Kernel privates
p M.private_instance_methods.include?(:puts)   # module does not
