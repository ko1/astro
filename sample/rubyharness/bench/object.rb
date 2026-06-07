class Pt
  def initialize(a, b); @a = a; @b = b; end
  def sum; @a + @b; end
end
s = 0; i = 0
while i < 4_000_000
  s += Pt.new(i, i).sum
  i += 1
end
p s
