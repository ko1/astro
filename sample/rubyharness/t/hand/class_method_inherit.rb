# class method inheritance (metaclass hierarchy) + inherited alias (rubyspec follow-up)
class CMParent
  def self.class_method; "I am #{name}"; end
  def self.greet; "hi from #{self}"; end
end
class CMChild < CMParent; end
class CMGChild < CMChild; end
p CMChild.class_method
p CMGChild.class_method
p CMChild.greet
p CMChild.respond_to?(:class_method)

class CMSib < CMParent
  def self.own; "own"; end
end
p CMSib.own
p CMSib.class_method

# alias_method of an inherited class method inside class << self
class CMChild3 < CMParent
  class << self
    alias_method :another_class_method, :class_method
  end
end
p CMChild3.another_class_method
