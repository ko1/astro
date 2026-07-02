# A Comparable object with no own <=> must not infinitely recurse
# (Comparable#== -> <=> -> Object#<=> -> == -> ...). vs ruby.
class NoCmp
  include Comparable
end
a = NoCmp.new; b = NoCmp.new
p (a == a)
p (a == b)
p (a == 5)
class HasEq
  include Comparable
  def initialize(v); @v = v; end
  def ==(o); o.is_a?(HasEq) && @v == o.instance_variable_get(:@v); end
end
p (HasEq.new(1) == HasEq.new(1))
p (HasEq.new(1) == HasEq.new(2))
# custom <=> still works
class Ok
  include Comparable
  def initialize(v); @v = v; end
  def <=>(o); @v <=> o.instance_variable_get(:@v); end
end
p (Ok.new(1) == Ok.new(1))
p (Ok.new(1) < Ok.new(2))
p [Ok.new(3), Ok.new(1), Ok.new(2)].sort.map { |x| x.instance_variable_get(:@v) }
# Object#<=> still uses == when defined
p (Object.new <=> Object.new)
