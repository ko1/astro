# #clone copies the singleton class without flushing method caches; call sites that
# already cached the original, private_constant behind its name filter, and a repeated
# require of a loaded feature behave as CRuby.  Run under both and diff.
o = Object.new
def o.f = 1
c = o.clone
p c.f
def o.f = 2
p o.f, c.f
def c.f = 3
p o.f, c.f
[o, c, o.clone, c.clone].each { p _1.f }
module M; def g = :m; end
o.extend(M)
d = o.clone
p d.g, d.singleton_class.include?(M)
module M; def g = :m2; end
p d.g, o.g
class << o; K = 5; end
e = o.clone
p e.singleton_class.const_get(:K), e.f
f = e.clone
def e.f = 9
p e.f, f.f
s = "str".clone
p s.frozen?, s.upcase

module P; X = 1; private_constant :X; Y = 2; end
p((P::X rescue $!.class), P::Y, P.const_get(:X))
module P; public_constant :X; end
p P::X
module Q; Y = 3; private_constant :Y; end
p((Q::Y rescue $!.class), P::Y)

p require("set"), require("json"), require("json"), $LOADED_FEATURES.count { _1.end_with?("/json.rb") }
