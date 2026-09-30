# Top-level calls from main with keyword / optional args, and send, while main has a singleton
# class; redefinition, singleton override, remove_method and refinements still apply.  Run under both and diff.
def fo(x = 1, *r) = [x, r]
3.times { p fo, fo(2), fo(3, 4) }
def fo(x = 9) = [:redef, x]
p fo
def self.fo(x = 7) = [:sing, x]
p fo
class << self; remove_method :fo; end
p fo
Object.send(:remove_method, :fo) rescue p $!.class
p((fo rescue [$!.class]))
module R; refine Object do; def rr(x = 1) = [:refined, x]; end; end
def rr(x = 1) = [:plain, x]
p rr
using R
p rr
def add(left:, right: 2) = left + right
p add(left: 1), add(left: 1, right: 5), (add(right: 1) rescue [$!.class, $!.message]), (add(left: 1, x: 2) rescue [$!.class, $!.message])
def self.add(left:, right:) = [:sing, left, right]
p add(left: 3, right: 4)
def kw2(a, *r, k: 0, **o) = [a, r, k, o]
p kw2(1, 2, k: 3, z: 4), kw2(1, **{k: 9})
class C; def m(x:) = x; def go = m(x: 5); end; p C.new.go
p puts_kw = format("%<a>d", a: 3)
def f0 = :top
p send(:f0), __send__(:f0), (public_send(:f0) rescue [$!.class, $!.message])
