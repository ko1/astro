# Class INHERITS from Module, but koruby registered ~56 Module methods on Class
# as well.  The copies SHADOWED the Module ones, so
#   class Module; public :remove_const; end        (pry's spec_helper)
# changed Module's entry while `SomeClass.remove_const` kept finding Class's
# private copy — and any redefinition of a Module method had the same problem.
# (`new` is special-dispatched in koruby and has no table entry, so it is
#  filtered out of this list.)
p (Class.instance_methods(false) - [:new]).sort
p Class.private_instance_methods(false).sort
p [Class.instance_method(:remove_const).owner, Module.instance_method(:remove_const).owner]
p [Class.instance_method(:remove_method).owner, Class.instance_method(:define_method).owner]
p [Class.instance_method(:private).owner, Class.instance_method(:attr_accessor).owner]

class Module
  public :remove_const
  public :remove_method
end
Object.const_set(:TmpTest, Module.new)
p Object.const_defined?(:TmpTest)
p Object.remove_const(:TmpTest)
p Object.const_defined?(:TmpTest)

# a redefinition on Module is seen by a Class receiver
module Module2Probe; end
class Module
  alias_method :__orig_ancestors, :ancestors
  def ancestors = [:patched]
end
class Zz; end
p [Zz.ancestors, Module2Probe.ancestors]
class Module
  alias_method :ancestors, :__orig_ancestors
  remove_method :__orig_ancestors
end
p Zz.ancestors.first(2)

# the module-only methods stay undefined on Class
%i[append_features prepend_features extend_object module_function refine].each do |m|
  p [m, (Class.instance_method(m) rescue $!.class)]
end
class UIM
  def gone; end
  undef_method :gone
end
p UIM.undefined_instance_methods
