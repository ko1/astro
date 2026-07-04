# Module#deprecate_constant is a no-op returning self (koruby emits no
# deprecation warning). vs ruby.
module M
  X = 1
  Y = 2
  deprecate_constant :X
end
p M::X
p M.deprecate_constant(:Y).equal?(M)
class C
  Z = 3
  deprecate_constant(:Z)
end
p C::Z
