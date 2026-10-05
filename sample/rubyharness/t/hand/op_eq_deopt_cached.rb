# == / != / === on user objects go through the operator node's inline cache: Ruby and C callees, redefinition,
# private / undef / method_missing, reflexive numeric and to_str forms, builtin subclasses, refinements
class Pt
  attr_reader :x
  def initialize(x) = @x = x
  def ==(o) = o.is_a?(Pt) && o.x == x
end
class Tag
  def ===(o) = o.to_s.start_with?("t")
end
class NoEq; end
class Sub < Array; def ==(o) = :sub_eq; end
class Str2 < String; end
class Num
  def ==(o) = o == 5 ? :five : false
end
class Strish
  def to_str = "abc"
  def ==(o) = o == "abc"
end
a = Pt.new(1); b = Pt.new(1); c = Pt.new(2); n1 = NoEq.new; n2 = NoEq.new
3.times do |i|
  GC.start if i == 1
  p a == b, a == c, a != b, a != c, a == a, n1 == n2, n1 != n2, n1 == n1
  p (case "tx" when Tag.new then :tag else :no end), (case "ax" when Tag.new then :tag else :no end)
  p Sub[1] == [1], Sub[1] != [1], Str2.new("a") == "a", 5 == Num.new, 5 != Num.new, "abc" == Strish.new, "abc" != Strish.new
  p [a, c].index(Pt.new(2)), [a, c].include?(Pt.new(3)), { a => 1 }.key?(b)
end
class Pt; def ==(o) = :redefined; end
p a == b, a != b
class Pt; def !=(o) = :neq_own; end
p a != b
class Pt; remove_method :!=; remove_method :==; end
p a == b, a == a, a != b
class Tag; def ===(o) = :new_eqq; end
p (case 1 when Tag.new then :hit end), Tag.new === 1
module R
  refine Pt do
    def ==(o) = :refined
  end
end
using R
p a == b
class MM; def method_missing(n, *a) = [n, a]; def respond_to_missing?(*) = true; end
p MM.new == 1, MM.new === 2
class Raiser; def ==(o) = raise(ArgumentError, "no eq"); end
begin
  Raiser.new == 1
rescue ArgumentError => e
  p e.message, e.backtrace_locations.first.label
end
