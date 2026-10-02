# respond_to? answers are cached per (class, name, include_all); every change
# that can move the answer must be seen, and a user respond_to_missing? is asked every time
class C
  def pub = 1
  private def priv = 2
  protected def prot = 3
end
o = C.new
3.times { p [o.respond_to?(:pub), o.respond_to?(:priv), o.respond_to?(:prot), o.respond_to?(:priv, true), o.respond_to?(:prot, true), o.respond_to?(:nope), o.respond_to?("pub")] }
class C; def later = 4; end
p o.respond_to?(:later)
class C; undef_method :pub; end
p o.respond_to?(:pub)
class C; public :priv; end
p o.respond_to?(:priv)
def o.single = 5
p o.respond_to?(:single), C.new.respond_to?(:single)
# a dynamic respond_to_missing? is asked on every call
class D
  attr_accessor :ok
  def respond_to_missing?(n, priv = false) = n == :dyn && ok
end
d = D.new
p [d.respond_to?(:dyn), (d.ok = true; d.respond_to?(:dyn)), (d.ok = false; d.respond_to?(:dyn))]
# builtin values
p ["s".respond_to?(:upcase), "s".respond_to?(:nope), 1.respond_to?(:+), nil.respond_to?(:to_a), :s.respond_to?(:length)]
class String; def shiny = :shiny; end
p "s".respond_to?(:shiny)
# Kernel private builtins with include_all
p [o.respond_to?(:puts), o.respond_to?(:puts, true)]
# a top-level def is a private method of every object
def toplevel_helper = 1
p [o.respond_to?(:toplevel_helper), o.respond_to?(:toplevel_helper, true)]
# class receivers share Class as the dispatch class until a metaclass exists
p [NilClass.respond_to?(:new), Object.respond_to?(:new), Integer.respond_to?(:new), Class.new.respond_to?(:new)]
# a refinement answers only where it is active
module RT
  refine String do
    def refined_only = 1
  end
end
def plain_ask(s) = s.respond_to?(:refined_only)
p plain_ask("a")
using RT
p "a".respond_to?(:refined_only), plain_ask("a")
