class Ver
  include Comparable
  attr_reader :n
  def initialize(n); @n = n; end
  def <=>(o); n <=> o.n; end
end
arr = Array.new(200) { |i| Ver.new((i * 7919) % 1000) }
s = 0; j = 0
while j < 3000
  sorted = arr.sort
  s += sorted.first.n
  j += 1
end
p s
