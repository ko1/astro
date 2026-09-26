# koruby's own `__` plumbing (C builtins, prelude helpers) is not listed by
# reflection — private_methods / private_instance_methods / methods show only
# what CRuby has — while a user's `__foo` methods are listed as usual.
p Object.new.private_methods.grep(/\A__/).sort
p Object.new.methods.grep(/\A__/).sort
p Kernel.private_instance_methods(false).grep(/\A__/).sort
p Class.new.singleton_class.private_instance_methods.grep(/\A__/).sort
p Module.new.private_instance_methods.grep(/\A__/)
class Foo
  def __mine = 1
  private def __priv = 2
end
p Foo.instance_methods(false), Foo.private_instance_methods(false), Foo.new.__mine
p Object.new.singleton_class.instance_methods.grep(/\A__/).sort
