words = "the quick brown fox jumps over the lazy dog"
s = 0; i = 0
while i < 300_000
  parts = words.split(" ")
  parts.each { |w| s += w.length }
  s += words.upcase.length
  s += words.index("fox") || 0
  i += 1
end
p s
