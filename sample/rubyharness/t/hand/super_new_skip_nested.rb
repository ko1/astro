module CI
  def new(*a, **k, &b)
    o = super
    o.instance_variable_set(:@m, {})
    o
  end
end
class Sch
  extend CI
  attr_reader :m
  def initialize(n) = @n = n
end
class Rel
  extend CI
  attr_reader :m, :sch
  def initialize = @sch = Sch.new(:x) { }   # nested `new` with a block (the generic send path) while the outer CI#new runs
end
r = Rel.new
p r.m, r.sch.m
class Task; def self.new(*a) = super; def initialize(x) = @x = x; end
class Lazy < Task; def self.new(*a) = super; end
p Lazy.new(1).class
module Safe; def new(*) = super.tap { |o| o.instance_variable_set(:@safe, true) }; end
class Q; extend Safe; def initialize = (@inner = Q2.new); attr_reader :inner; end
class Q2; extend Safe; end
q = Q.new; p q.instance_variable_get(:@safe), q.inner.instance_variable_get(:@safe)
class RR < Range; def self.new(a, b) = super(a, b, false); end
p RR.new(1, 3).to_a
