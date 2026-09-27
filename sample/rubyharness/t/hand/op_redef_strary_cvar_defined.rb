# A user object's own * / + - % is called even when the argument is a Complex;
# String/Array operators reopened (or prepended) after boot are honoured by the
# `a + b` / `"%s" % x` / `a < b` call sites; defined?(@@x) in an extended
# module's method resolves like the read.
class RU
  %i[* / - + %].each { |op| define_method(op) { |o| [op, o] } }
end
%i[* / - + % ** <=> ==].each { |op| p((eval("RU.new #{op} Complex(1, 1)") rescue $!.class)) }
p [(Object.new * Complex(1, 2) rescue $!.class), Complex(1, 2) * 2]
module RPct; def %(*a) = a.first == :x ? "prepended" : super; end
class String; prepend RPct; end
p "%s" % :x, "%s" % :y, "%s".send(:%, :x)
p 5 % 3, 2.5 % 2, 1 + 2, "a" + "b"
class Array; alias __orig_plus +; def +(o) = __orig_plus(o).push(:plus); end
p [1] + [2], [1] - [1], [1] * 2
class String; alias __orig_lt <; def <(o) = [:lt, __orig_lt(o)]; end
p "a" < "b", ("a" <= "b"), 1 < 2
module RIntMod; def %(o) = [:mod, super]; end
class Integer; prepend RIntMod; end
p 7 % 3, 7.0 % 3
module CM
  def set = (@@flag = 1)
  def get = defined?(@@flag)
  def get2 = @@flag
  def get3 = defined?(@@nope)
end
class CMFoo; extend CM; end
CMFoo.set
p CMFoo.get, CMFoo.get2, CMFoo.get3, CM.class_variables
