# `when SomeClass` answers is_a? without sending === while Module#=== is the builtin;
# a class with its own === (or a redefinition later) must still be honoured.
module Tag; end
class Base; end
class Sub < Base; include Tag; end
def kind(v)
  case v
  when String then :str
  when Integer then :int
  when Sub then :sub
  when Base then :base
  when Tag then :tag
  when Comparable then :cmp
  when nil then :nil
  else :other
  end
end
vals = ["s", 1, Sub.new, Base.new, Object.new.extend(Tag), 1.5, nil, :sym, [1]]
3.times { p vals.map { kind(_1) } }

class Matcher
  def self.===(o) = o.is_a?(Integer) && o > 10
end
def big(v) = case v when Matcher then :big else :small end
p [big(5), big(50), big("x")]

# a later singleton === on a class used in `when`
def str_kind(v) = case v when String then :yes else :no end
p [str_kind("a"), str_kind(1)]
class String
  def self.===(o) = o == :pretend
end
p [str_kind("a"), str_kind(:pretend)]
# Module#=== itself redefined
class Module
  alias_method :orig_eqq, :===
  def ===(o) = o == :anything || orig_eqq(o)
end
p [kind(:anything), kind(3), kind(Sub.new)]
