class U; def meth = 1; def m2 = 2; def m3 = 3; undef :"#{'meth'}"; end; p U.instance_methods(false).sort
class U; undef :"#{'m2'.to_sym}", m3; end; p U.instance_methods(false)
class V; def a = 1; def c = 3; end
p((class V; undef a, :"#{'nope'}", c; end rescue $!.class)); p V.instance_methods(false).sort
class W < V; undef :"#{'c'}"; end; p W.new.respond_to?(:c), V.new.respond_to?(:c)
