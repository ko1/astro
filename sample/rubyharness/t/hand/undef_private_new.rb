# `undef_method :new` and `private_class_method :new` are visible to dispatch
# and to reflection, even though .new has no ordinary method-table entry.
class K1
  class << self
    undef_method :new
  end
end
p [K1.respond_to?(:new), (K1.new rescue [$!.class, $!.message[0, 40]])]
p K1.singleton_class.method_defined?(:new)

class K2; end
K2.singleton_class.send(:undef_method, :new)
p [K2.respond_to?(:new), (K2.new rescue $!.class)]

class K3
  private_class_method :new
  def self.build = new
  def initialize(a = 1) = @a = a
  def a = @a
end
p [K3.respond_to?(:new), K3.respond_to?(:new, true)]
p (K3.new rescue [$!.class, $!.message[0, 40]])
p K3.build.a
K3.public_class_method :new
p [K3.respond_to?(:new), K3.new(2).a]

require 'singleton'
class S1
  include Singleton
  def v = 42
end
p [S1.respond_to?(:new), S1.instance.v, (S1.new rescue $!.class)]

# a plain class is unaffected
class K4; def initialize(x = 0) = @x = x; def x = @x; end
p [K4.respond_to?(:new), K4.new(3).x, K4.singleton_class.ancestors.include?(Class)]
# Struct / builtin subclasses keep working
S2 = Struct.new(:a, :b)
p S2.new(1, 2).to_a
class MyStr < String; end
p MyStr.new("hi").upcase
