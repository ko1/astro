# a module that gains an include / prepend after classes already include it: the includers see it (CRuby >= 3.0)
module M; def m = [:M] + (defined?(super) ? super : []); end
class Base; def m = [:Base]; end
class C < Base; include M; end
class D < C; end
c = C.new; d = D.new
p c.m, d.m
module N; def m = [:N] + super; def n = :n; end
M.include N
p C.ancestors.take(5), c.m, d.m, d.n, D.instance_method(:n).owner
module P; def m = [:P] + super; end
M.prepend P
p C.ancestors.take(6), c.m, d.m
s = C.new
s.extend(Module.new { def m = [:S] + super })
p s.m, s.singleton_class.ancestors.take(4).map { _1.name || :anon }
module Q; def q = :q; end
N.include Q                                       # two levels down
p d.q, D.include?(Q), D.ancestors.index(Q) > D.ancestors.index(N)
module M; undef_method :m; end                   # undef in the module after inclusion
p((d.m rescue :nomethod))
p C.ancestors.take(7)
module PP; end
class X; prepend PP; end
p X.include?(PP), X.include?(Kernel), M.include?(M), M.include?(Q), Comparable.include?(Comparable)
