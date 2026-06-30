# Array#values_at coerces Range bounds via #to_int (bounds must also be
# Comparable so the Range is valid). vs ruby.
class TI
  include Comparable
  def initialize(n); @n = n; end
  def to_int; @n; end
  def <=>(o); @n <=> o.to_int; end
end
a = [1, 2, 3, 4, 5]
p a.values_at(TI.new(1)..TI.new(3))
p a.values_at(TI.new(0)...TI.new(2))
p a.values_at(1..3)
p a.values_at(1...3)
p a.values_at(0, 2, 4)
p a.values_at(-2..-1)
