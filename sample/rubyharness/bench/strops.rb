WORDS = "the quick brown fox jumps over the lazy dog"
INNER = 3_000
OUTER = 100

def bench
  s = 0; i = 0
  while i < INNER
    parts = WORDS.split(" ")
    parts.each { |w| s += w.length }
    s += WORDS.upcase.length
    s += WORDS.index("fox") || 0
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
