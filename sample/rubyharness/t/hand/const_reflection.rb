# Module#const_get / const_set / const_defined? are namespace-aware. vs ruby.
module M
  X = 1
  module Inner; Y = 2; end
end
p M.const_get(:X)
p M.const_get("Inner::Y")
p M.const_defined?(:X)
p M.const_defined?(:Nope)
M.const_set(:Z, 99)
p M::Z
p M.constants.sort
p M.constants.include?(:Z)
class Base9; C1 = 1; end
class Deriv9 < Base9; C2 = 2; end
p Deriv9.const_get(:C1)
p Deriv9.const_defined?(:C1)
p Deriv9.const_defined?(:C1, false)
p Deriv9.const_defined?(:C2, false)
begin; M.const_get(:Missing); rescue NameError => e; p e.class; end
# a colliding top-level const does not shadow the module's own
STATUS = :top
module Cfg; STATUS = :cfg; end
p Cfg.const_get(:STATUS)
p STATUS
