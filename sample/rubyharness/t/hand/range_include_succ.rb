class TS
  attr_reader :n
  def initialize(n); @n = n; end
  def succ; TS.new(@n * 10); end
  def <=>(o); @n <=> o.n; end
  def ==(o); o.is_a?(TS) && @n == o.n; end
end
r = TS.new(1)..TS.new(99)
p r.include?(TS.new(1))
p r.include?(TS.new(10))
p r.include?(TS.new(2))
p r.include?(TS.new(0))
p r.member?(TS.new(100))
p((1..100).include?(50))
p(('a'..'c').include?('b'))
p(('a'..'c').include?('bc'))
