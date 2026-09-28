# singleton_methods lists methods of modules prepended to the singleton class
module P; def inherited(s) = super; def pm = 1; end
module Q; def qm = 2; end
class K; end
K.singleton_class.prepend(P)
K.extend(Q)
p K.singleton_methods.sort, K.singleton_methods(false).sort
o = Object.new; o.singleton_class.prepend(P); def o.own = 1
p o.singleton_methods.sort, o.singleton_methods(false).sort
o2 = Object.new
o2.singleton_class.prepend(P)
o2.extend(Q)
def o2.own = 1
p o2.singleton_methods
