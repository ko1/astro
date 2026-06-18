def bench
  s = 0; i = 0
  while i < 4_000
    h = { a: [1, 2, { b: i }], c: { d: [i, i + 1] } }
    s += h[:a][2][:b] + h[:c][:d][1]
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
