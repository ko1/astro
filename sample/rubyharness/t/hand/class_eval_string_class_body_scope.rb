# A class body in a class_eval / module_eval string nests under the receiver, even when
# a same-named class exists elsewhere; Module.nesting resolves each level under the last.
module R; class A; end; class B; end
  A.module_eval "class T; p Module.nesting; Y = 1; end"
  B.module_eval "class T; p Module.nesting; Z = 1; end"
  p A::T.constants, B::T.constants
end
module M; class T; end; end
class D; class T; p Module.nesting; W = 1; end; end
p D::T.constants, M::T.constants
class Q; end; Q.module_eval "class T; Q1 = 1; end"; p Q::T.constants
