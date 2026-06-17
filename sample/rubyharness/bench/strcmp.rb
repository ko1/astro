words = %w[banana apple cherry date elderberry fig grape kiwi lemon mango]
s = 0; i = 0
while i < 200_000
  sorted = words.sort
  s += sorted.first.length + sorted.last.length
  s += 1 if words.include?("kiwi")
  i += 1
end
p s
