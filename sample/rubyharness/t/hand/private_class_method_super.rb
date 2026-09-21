# private_class_method on an INHERITED class method copies it down; `super` from
# the copy must resume after the module that really defined it.
module Wrap
  def new(*args, &block)
    r = super
    ($log ||= []) << :wrapped
    r
  end
end
class Base
  extend Wrap
  def initialize(a = 0) = @a = a
  def a = @a
end
class Sub < Base
  private_class_method :new
  def self.make(x) = new(x)
end
p Sub.make(3).a
p $log
p (Sub.new(1) rescue [$!.class, $!.message[0, 40]])
p Base.new(2).a
p [Sub.singleton_class.ancestors.first(3).map(&:to_s)]
p Sub.method(:make).owner.to_s

# public_class_method restores it
Sub.public_class_method :new
p Sub.new(4).a
p $log.size

# the instance-method form keeps working too
class C2
  def hidden = "h"
end
class C3 < C2
  public :hidden
end
p C3.new.hidden
module IM
  def val = "IM" + (defined?(super) ? super : "")
end
class D1; def val = "D1"; end
class D2 < D1; include IM; end
class D3 < D2; private :val; def pub = val; end
p D3.new.pub
