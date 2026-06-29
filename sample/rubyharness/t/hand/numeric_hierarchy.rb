p Integer.ancestors
p Float.ancestors
p Numeric.ancestors
p Numeric.class
p Integer.superclass
p Float.superclass
p Numeric.superclass
p Rational.superclass
p 5.is_a?(Numeric)
p 5.is_a?(Comparable)
p 1.5.is_a?(Numeric)
p Numeric === 5
p Comparable === 5
class MyNum < Numeric
  def initialize(v); @v = v; end
  def <=>(o); @v <=> o.instance_variable_get(:@v); end
end
a = MyNum.new(3)
p a.is_a?(Numeric)
p a.is_a?(Comparable)
p a.is_a?(Object)
p a < MyNum.new(5)
p MyNum.ancestors.include?(Object)
p MyNum.superclass
