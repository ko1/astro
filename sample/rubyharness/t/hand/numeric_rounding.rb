# Numeric mixin provides floor/ceil/round/truncate that convert self via #to_f
# and delegate; Integer/Float keep their own (more precise) versions. vs ruby.
class Deg < Numeric
  def initialize(f); @f = f; end
  def to_f; @f; end
end
p Deg.new(2.7).floor
p Deg.new(2.7).ceil
p Deg.new(2.4).round
p Deg.new(2.7).truncate
p Deg.new(-2.7).floor
p Deg.new(-2.7).ceil
p Deg.new(-2.7).truncate
# Integer / Float precedence unaffected
p 3.14.floor
p 3.14.ceil
p 3.7.round
p (-3.7).truncate
p 100.floor
p 100.ceil
p 3.14159.round(2)
p 3.14159.floor(3)
p 5.round(-1)
