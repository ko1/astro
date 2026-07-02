# `expr::CONST` (constant path with a non-constant parent) resolves via the
# module's constants + ancestors. vs ruby.
module M
  X = 42
  module Inner; Y = 7; end
end
mod = M
p mod::X
inner = M::Inner
p inner::Y
# ancestor lookup through the parent expression
class Base; C = "base_c"; end
class Sub < Base; end
s = Sub
p s::C
# missing constant raises NameError
begin
  m = M
  m::Missing
rescue NameError => e
  p e.class
end
