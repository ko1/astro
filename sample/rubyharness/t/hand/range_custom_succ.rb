# A Range over a custom Comparable object that defines #succ iterates via succ. vs ruby.
class V
  include Comparable
  attr_reader :n
  def initialize(n); @n = n; end
  def <=>(o); @n <=> o.n; end
  def succ; V.new(@n + 1); end
end
p((V.new(1)..V.new(4)).map(&:n))
p((V.new(1)...V.new(4)).map(&:n))
p((V.new(3)..V.new(1)).map(&:n))
p((V.new(2)..V.new(2)).to_a.size)
