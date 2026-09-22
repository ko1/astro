# Symbol id 0 is the "no symbol" sentinel all over the runtime (an anonymous
# parameter, an unnamed class, `super` outside a method body).  The FIRST name
# interned used to take it, and that was `:puts` — so anything named `puts`
# tripped every one of those tests.
def m1(puts); end
p method(:m1).parameters
def m2(a, puts, b); end
p method(:m2).parameters
p ->(puts) {}.parameters
p ->(a, puts = 1, *puts2, key: 2, &puts3) {}.parameters
def m3(puts) = binding.local_variables
p m3(1)

class BaseP
  def puts(*a) = [:base, a]
  def print(*a) = [:base_print, a]
end
class SubP < BaseP
  def puts(*a) = super
  def print(*a) = super
end
p [SubP.new.puts(1), SubP.new.print(2)]

# a class / module named after the first-interned symbol is impossible (constants
# are capitalised), but an ivar and a method both are
class IvarPuts
  def initialize = @puts = :iv
  attr_reader :puts
end
p [IvarPuts.new.puts, IvarPuts.new.instance_variables]

# `:""` still gets an id of its own, and all_symbols never hands out the sentinel
p ["".to_sym, "".to_sym == :"", Symbol.all_symbols.include?(:"")]
p Symbol.all_symbols.all? { |s| s.is_a?(Symbol) }
