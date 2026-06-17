s = 0; i = 0
while i < 2_000_000
  str = "row #{i & 1023} = #{(i * 7) & 4095} (#{i.even? ? "e" : "o"})"
  s += str.length
  i += 1
end
p s
