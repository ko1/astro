INNER = 20_000
OUTER = 100

def bench
  s = 0; i = 0
  while i < INNER
    str = "row #{i & 1023} = #{(i * 7) & 4095} (#{i.even? ? "e" : "o"})"
    s += str.length
    i += 1
  end
  s
end

result = 0
i = 0
while i < OUTER
  result = bench
  i += 1
end
p(result)
