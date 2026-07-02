# Module#const_get: scoped names, inherit flag, to_str coercion, const_missing,
# leading ::. vs ruby.
module M
  X = 1
  module Inner; Y = 2; end
end
p M.const_get(:X)
p M.const_get("Inner::Y")
p M.const_get("::M")
p Object.const_get("M::Inner::Y")
class Base; C = 10; end
class Sub < Base; end
p Sub.const_get(:C)
begin; Sub.const_get(:C, false); rescue NameError; p :no_inherit; end
class NmStr; def to_str; "X"; end; end
p M.const_get(NmStr.new)
class HasCM; def self.const_missing(n); "cm_#{n}"; end; end
p HasCM.const_get(:Whatever)
begin; M.const_get(:Zzz); rescue NameError; p :ne; end
