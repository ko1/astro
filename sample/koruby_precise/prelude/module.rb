# Module/Class introspection layered on the C core (ancestors/instance_method/
# attached_object).
class Module
  # The modules in the ancestor chain that are not classes.
  def included_modules
    ancestors.select { |m| m.instance_of?(Module) }
  end

  # koruby does not track method visibility strictly; public_instance_method is
  # instance_method for a public/protected method (a private one should raise,
  # but the distinction isn't modelled here).
  def public_instance_method(name)
    instance_method(name)
  end

  # True iff self is a singleton class.  attached_object succeeds only for a
  # singleton class (raises TypeError otherwise), so use it as the predicate.
  def singleton_class?
    attached_object
    true
  rescue TypeError
    false
  end

  # koruby has no refinements; report none.
  def refinements; []; end
  def undefined_instance_methods; []; end

  # Default hooks (private).  const_added is a no-op; const_missing raises.
  # (The automatic firing of these hooks is a separate C concern.)
  def const_missing(name)
    msg = self.equal?(Object) ? "uninitialized constant #{name}" : "uninitialized constant #{self}::#{name}"
    raise NameError.new(msg, name, receiver: self)   # NameError#name / #receiver reflect the missing constant
  end
  private def const_added(name); end

  # ruby2_keywords marks methods to pass a bare kwargs hash through *args.  koruby
  # already threads a trailing kwargs Hash through splat, so accept + no-op.
  def ruby2_keywords(*names); nil; end

  # The primitive behind Object#extend (private): mix self into obj's singleton class.
  private def extend_object(obj)
    obj.singleton_class.include(self)
    obj
  end
end
