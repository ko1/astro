# koruby names its internal helpers __foo.  CRuby exposes only Kernel's
# __callee__ / __dir__ / __method__ (plus __id__ / __send__), so anything else
# showing up in #methods / #respond_to? / #instance_methods is noise a gem doing
# method introspection can trip over.
require "set"

p Object.new.methods.grep(/\A__/).sort
p "s".methods.grep(/\A__/).sort
p [].methods.grep(/\A__/).sort
p({}.methods.grep(/\A__/).sort)
p Set.new.methods.grep(/\A__/).sort
p (1..2).methods.grep(/\A__/).sort
p 1.methods.grep(/\A__/).sort
p :s.methods.grep(/\A__/).sort
p [].each.methods.grep(/\A__/).sort
p Class.new.methods.grep(/\A__/).sort
p RuntimeError.new("x").methods.grep(/\A__/).sort
p //.methods.grep(/\A__/).sort
p Thread.current.methods.grep(/\A__/).sort
p $stdout.methods.grep(/\A__/).sort

# the public ones stay public
p Object.new.respond_to?(:__id__), Object.new.respond_to?(:__send__)
p 1.__id__.is_a?(Integer), 1.__send__(:to_s)

# Enumerable / Comparable mixins add nothing public
p Enumerable.instance_methods.grep(/\A__/).sort
p Comparable.instance_methods.grep(/\A__/).sort
# koruby defines __send__ / __id__ on Kernel, CRuby on BasicObject — both reach
# every object, so compare the rest.
p (Kernel.instance_methods.grep(/\A__/) - [:__send__, :__id__]).sort

# a user's own __name is untouched
class UserDefined
  def __mine = 1
end
p UserDefined.new.__mine, UserDefined.new.respond_to?(:__mine)
p UserDefined.instance_methods(false)
