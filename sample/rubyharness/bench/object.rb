class Pt
  def initialize(a, b); @a = a; @b = b; end
  def sum; @a + @b; end
end

def bench
  s = 0; i = 0
  while i < 40_000
    s += Pt.new(i, i).sum
    i += 1
  end
  s
end

result = 0
i = 0
while i < 100
  result = bench
  i += 1
end
p result
