class Ver
  include Comparable
  attr_reader :n
  def initialize(n); @n = n; end
  def <=>(o); n <=> o.n; end
end

def bench
  arr = Array.new(200) { |i| Ver.new((i * 7919) % 1000) }
  sorted = arr.sort
  sorted.first.n
end

result = 0
i = 0
while i < 3000
  result = bench
  i += 1
end
p(result)
