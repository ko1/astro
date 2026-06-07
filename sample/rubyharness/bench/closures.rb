def adder(n) = ->(x) { x + n }
s = 0; i = 0
while i < 3_000_000
  f = adder(i)
  s += f.call(1)
  i += 1
end
p s
