# #respond_to? skips dispatching the default Kernel#respond_to_missing? (always false), but still
# asks an override: in a class, a module, a singleton, Kernel reopened, and after remove_method.
class Plain; end
class Dyn
  def respond_to_missing?(name, priv = false) = name.to_s.start_with?("dyn_") || (priv && name == :secret)
end
module M
  def respond_to_missing?(name, priv = false) = name == :from_m || super
end
class WithM; include M; end
calls = 0
o = Object.new
o.define_singleton_method(:respond_to_missing?) { |n, p = false| calls += 1; n == :single }
p Plain.new.respond_to?(:foo), Plain.new.respond_to?(:to_s), Plain.new.respond_to?(:puts), Plain.new.respond_to?(:puts, true)
p Dyn.new.respond_to?(:dyn_x), Dyn.new.respond_to?(:other), Dyn.new.respond_to?(:secret), Dyn.new.respond_to?(:secret, true)
p WithM.new.respond_to?(:from_m), WithM.new.respond_to?(:nope)
p o.respond_to?(:single), o.respond_to?(:other), calls
p 1.respond_to?(:zzz), nil.respond_to?(:zzz), "s".respond_to?(:zzz), [].respond_to?(:zzz)
def loop_site(x) = x.respond_to?(:dyn_y)
p [Plain.new, Dyn.new, Plain.new, Dyn.new].map { loop_site(_1) }
module Kernel
  def respond_to_missing?(name, priv = false) = name == :kernel_wide
end
p Plain.new.respond_to?(:kernel_wide), Plain.new.respond_to?(:foo), loop_site(Plain.new)
class Dyn; remove_method :respond_to_missing?; end
p Dyn.new.respond_to?(:dyn_x), Dyn.new.respond_to?(:kernel_wide)
