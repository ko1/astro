def bench
  s = 0
  i = 0
  while i < 100_000
    begin
      raise "boom" if i & 7 == 0
      s += 1
    rescue => e
      s += e.message.length
    end
    i += 1
  end
  s
end

result = 0
i = 0
while i < 20
  result = bench
  i += 1
end
p(result)
