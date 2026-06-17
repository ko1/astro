s = 0; i = 0
while i < 400_000
  h = { a: [1, 2, { b: i }], c: { d: [i, i + 1] } }
  s += h[:a][2][:b] + h[:c][:d][1]
  i += 1
end
p s
