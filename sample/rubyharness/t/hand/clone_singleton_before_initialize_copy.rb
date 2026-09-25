# #clone copies the singleton class before initialize_copy runs (and the copied
# singleton makes initialize & co. private, as CRuby's clone does).
o = Object.new; def o.hi = 1
def o.initialize_copy(s) = (p [:ic, singleton_methods.sort]; super)
c = o.clone; p c.singleton_methods.sort, c.hi
d = o.dup; p d.singleton_methods
s = +"str"; def s.x = 2; s2 = s.clone; p s2.x, s2.singleton_methods
a = [1]; a.define_singleton_method(:y) { 3 }; p a.clone.y
class K; def initialize_copy(o) = (p [:k, respond_to?(:z)]); end
k = K.new; def k.z = 1; k.clone; k.dup
f = Object.new.freeze; def (g = Object.new).w = 1; p g.clone(freeze: true).frozen?, g.clone.w
o = Object.new
def o.initialize_copy(x) = 1
def o.initialize = 1
def o.respond_to_missing?(*) = false
def o.initialize_dup(x) = 1
p o.singleton_class.private_instance_methods(false).sort, o.singleton_methods.sort
class << o; def initialize_clone(x) = 1; end
p o.singleton_class.private_method_defined?(:initialize_clone)
