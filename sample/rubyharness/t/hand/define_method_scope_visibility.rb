# define_method / attr_* take the CALLING scope's default visibility: a bare private in the body does not
# reach one run from a method (parslet rule, ai4r parameters_info)
module Rules; def rule(n) = define_method(n) { n }; end
class A
  extend Rules
  private
  def self.mk(n) = define_method(n) { n }
  mk(:x)
  rule(:y)
  define_method(:z) { 1 }
  [:w].each { |n| define_method(n) { 2 } }
  class_eval { define_method(:v) { 3 } }
end
p A.public_method_defined?(:x), A.public_method_defined?(:y), A.private_method_defined?(:z), A.private_method_defined?(:w), A.private_method_defined?(:v)
class B
  private
  send(:define_method, :s) { 1 }
  public_send(:define_method, :t) { 1 } rescue nil
end
p B.private_method_defined?(:s)
module PI; def params(*ns) = ns.each { attr_accessor _1 }; end
class A
  extend PI
  protected
  params(:lr)
  attr_accessor :direct
  [:blk].each { attr_reader _1 }
end
p A.public_method_defined?(:lr=), A.protected_method_defined?(:direct), A.protected_method_defined?(:blk)
class B; private; attr_writer :w; end; p B.private_method_defined?(:w=)
