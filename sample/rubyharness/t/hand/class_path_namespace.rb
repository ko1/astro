# `class M::C` / `module M::Inner` (constant-path form) carry the qualified name
# and register under the parent's Module#constants, like the lexical form. vs ruby.
module M; end
class M::C; end
class M::D < M::C; end
module M::Inner; end
class M::Inner::E; end
X_IN_M = nil
p M::C.name
p M::D.name
p M::D.superclass.name
p M::Inner.name
p M::Inner::E.name
p M::C.new.class.name
p "#{M::C}"
p M.constants.sort
p M::Inner.constants
begin; M::C.new.nope; rescue NoMethodError => e; p e.message; end
# lexical form unaffected
module Lex; class Y; end; Z = 1; end
p Lex::Y.name
p Lex.constants.sort
