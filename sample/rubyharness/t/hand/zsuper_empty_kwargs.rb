# A bare `super` from a method that HAS keyword parameters forwards them — but an
# EMPTY forwarded kwargs Hash is dropped, like any other `**{}` call site.  Without
# the drop the parent sees a stray positional `{}` (dry-core's Memoizable wraps
# `new` with `def new(*, **)` and every `Memoized.new` raised ArgumentError).
class P1; def m; :p; end; end
class C1 < P1; def m(*, **) = super; end
class C2 < P1; def m(*a, **k) = super; end
class C3 < P1; def m(**) = super; end
class C4 < P1; def m(**k) = super; end
p [C1.new.m, C2.new.m, C3.new.m, C4.new.m]

module M5; def m; :mod; end; end
class C5; include M5; def m(*, **) = super; end
p C5.new.m

# the same shape on a singleton — this is the `new` wrapper dry-core installs
module CI; def new(*, **); o = super; o.instance_variable_set(:@z, 1); o; end; end
class N0; extend CI; end
class N1; extend CI; def initialize; @a = 1; end; end
class N2; extend CI; def initialize(x, k: 2); @x = x; @k = k; end; end
p [N0.new.instance_variable_get(:@z), N1.new.instance_variable_get(:@z),
   N2.new(1).instance_variable_get(:@k), N2.new(1, k: 9).instance_variable_get(:@k)]

# a NON-empty forwarded kwargs Hash still reaches the parent as keywords
class P6; def m(a = nil, k: :none, **rest) = [a, k, rest]; end
class C6 < P6; def m(*, **) = super; end
class C7 < P6; def m(a = nil, k: :none, **rest) = super; end
p [C6.new.m, C6.new.m(1), C6.new.m(1, k: 2), C6.new.m(k: 2, z: 3),
   C7.new.m(1, k: 2), C7.new.m]

# explicit `super(**h)` with an empty h drops too, and a plain positional Hash does not
class C8 < P6; def m(h) = super(**h); end
class C9 < P6; def m(h) = super(h); end
p [C8.new.m({}), C8.new.m({k: 5}), C9.new.m({}), C9.new.m({k: 5})]
