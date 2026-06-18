WORDS = %w[banana apple cherry date elderberry fig grape kiwi lemon mango]
INNER = 2_000
OUTER = 100

def bench
  s = 0; i = 0
  while i < INNER
    sorted = WORDS.sort
    s += sorted.first.length + sorted.last.length
    s += 1 if WORDS.include?("kiwi")
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
