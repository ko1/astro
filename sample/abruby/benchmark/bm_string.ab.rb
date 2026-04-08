# String concatenation
i = 0
s = ""
while i < 120_000
  s += "x"
  i += 1
end
p(s.length)
