# String concatenation
def bench
  i = 0
  s = ""
  while i < 1700
    s += "x"
    i += 1
  end
  s.length
end

result = 0
i = 0
while i < 1000
  result = bench
  i += 1
end
p(result)
