def classify(n)
  case n % 7
  when 0 then 10
  when 1 then 20
  when 2 then 30
  when 3 then 40
  when 4 then 50
  when 5 then 60
  else 70
  end
end

def bench
  s = 0
  i = 0
  while i < 100_000
    s += classify(i)
    i += 1
  end
  s
end

result = 0
i = 0
while i < 80
  result = bench
  i += 1
end
p(result)
