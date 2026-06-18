def bench
  f = 1
  i = 1
  while i < 500
    f = f * i
    i += 1
  end
  f % 1_000_000_007
end

result = 0
i = 0
while i < 1500
  result = bench
  i += 1
end
p(result)
