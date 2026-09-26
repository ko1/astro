# A top-level def is a private Object instance method (pry's Pry::Method#visibility
# reads it from Object.private_instance_methods); a bare `public` above it or a
# def nested in a method body makes it public.
def tl = :tl
p method(:tl).owner, Object.private_method_defined?(:tl), Object.public_method_defined?(:tl)
p Object.private_instance_methods(false).include?(:tl), Object.public_instance_methods.include?(:tl)
p((Object.new.tl rescue $!.message), 1.send(:tl), (1.public_send(:tl) rescue $!.class))
p Object.new.respond_to?(:tl), Object.new.respond_to?(:tl, true)
class Foo; def m = tl; end
p Foo.new.m, Foo.private_method_defined?(:tl), Foo.private_method_defined?(:tl, false)
p Kernel.private_method_defined?(:tl), BasicObject.private_method_defined?(:tl)
p Object.instance_method(:tl).owner
def rm = 1
Object.send(:remove_method, :rm)
p((rm rescue $!.class))
def outer
  def nested_in_method = :n
end
outer
p Object.public_method_defined?(:nested_in_method), Object.new.nested_in_method
public
def pub_tl = :pub
private
p Object.public_instance_methods(false).include?(:pub_tl), Object.new.pub_tl
def priv_again = 1
p Object.private_method_defined?(:priv_again)
p Class.new.instance_method(:tl).owner, Class.new.new.method(:tl).unbind, 1.method(:tl)
class G; define_method(:mk) { def made_in_dm = :g; 1 }; end
G.new.mk
p G.public_method_defined?(:made_in_dm), Object.private_method_defined?(:made_in_dm)
prc = nil
t = Class.new { prc = -> { def from_lambda = 42 } }
c = Class.new { define_method(:test, prc) }
o = c.new; o.test
p o.respond_to?(:from_lambda), t.new.from_lambda
