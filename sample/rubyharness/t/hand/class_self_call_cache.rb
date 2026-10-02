# an implicit-self call inside a class / module method caches on self itself:
# different selfs through one site, redefinitions and extends must all be seen
class P
  def self.helper = :p_helper
  def self.run = helper
end
class Q < P
  def self.helper = :q_helper
end
class R < P; end
3.times { p [P.run, Q.run, R.run] }
class R
  def self.helper = :r_helper
end
p [P.run, Q.run, R.run]
module Ext; def helper = :ext_helper; end
class S < P; end
p S.run
S.singleton_class.prepend(Ext)
p S.run
module M
  def self.a(x) = b(x) + 1
  def self.b(x) = x * 2
end
p [M.a(1), M.a(2)]
module M
  def self.b(x) = x * 10
end
p [M.a(1), M.a(2)]
# a module_function method called both ways
module F
  module_function
  def inner = :inner
  def outer = inner
end
p F.outer
class UsesF; include F; def go = outer; end
p UsesF.new.go
# one site reached from a class self and from its instances (dry-cli Style: include + extend
# of the same builder module whose methods call `add`)
module Builders
  def red = add(:red)
end
class Style
  include Builders
  extend Builders
  def self.add(x) = [:class, x, new.class]
  def add(x) = [:instance, x]
end
3.times { p [Style.red, Style.new.red, Style.red] }
