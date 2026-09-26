# Forwardable delegators forward keywords as keywords (they use `...`, as CRuby's do).
require 'forwardable'
class P; def self.parse(s, **o, &b) = [s, o, b&.call]; def self.pos(*a) = a; def self.h(h) = h; end
class A
  extend Forwardable
  def parser = P
  instance_delegate [:parse, :pos, :h] => :parser
  def_delegator :parser, :parse, :parse2
  def_delegator :@arr, :first
  def initialize = @arr = [9]
end
a = A.new
p a.parse("x", pattern: 1), a.parse("y") { 3 }, a.pos(1, 2), a.h({k: 1}), a.h(k: 2), a.parse2("z", q: 1), a.first
module S; extend SingleForwardable; def_delegator :"P", :pos; end
p S.pos(1, k: 2)
